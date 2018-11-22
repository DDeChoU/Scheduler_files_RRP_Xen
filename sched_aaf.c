/************** AAF ******************
* Developers : Pavan Kumar Paluri & Guangli Dai
* AAF: Algorithm that creates fixed partitions and assugns them
* to PCPUs based on the concept of levels and AAF
* Years : 2018-2109
* **********************************/
#define __AAF_SINGLE__
#define __AAF_HP__
#include <xen/sched.h>
#include <xen/sched-if.h>
#include <xen/softirq.h>
#include <xen/keyhandler.h>
#include <xen/trace.h>
#include <xen/lib.h>
#include <xen/time.h>
#include <xen/domain.h>
#include <xen/init.h>
#include <xen/config.h>
#include <xen/event.h>
#include <xen/perfc.h>
#include <xen/errno.h>
#include <xen/cpu.h>
#include <xen/guest_access.h>
#include <math.h>
/* Macros for debugging purposes */
#ifndef CHECK(_p)
#define CHECK(_p) \
do{if(!(_p) \
printk("Checking of '%s' failed at line %d of file %s\n", \
#_p,__LINE__,__FILE__);	\
} while(0) 

#else
#define CHECK(_p) (void 0)
#endif

/* Macros for Binary Operations */
#define EQUAL(a,b) ((!!a)==(!!b))
#define IMPLY(a,b) ((!a)||(b))
#define PWR_TWO(a) (1<< (a))
#define LN10 2.3025850929940456840179914546844
#define LOG10(x) (ln(x)/LN10)
/* Macros for scheduling purposes */
#define QUANTUM (MILLISECS(100))
/* being hard-coded now, make it dynamic for later use */
#define NUMBER_OF_DOMAINS (5)
#define NUMBER_OF_LEVELS (5)
#define NUMBER_OF_SLICES_PER_LEVEL (4096)
#define SLICE_ARRAY_SIZE (65536)
#define AAF_SLEEP (MILLISECS(20))
#define PERIOD_MIN (MICROSECS(10))
#define PERIOD_MAX (MILLISECS(10000))
#define AAF_COMP_MIN (MICROSECS(5))
#define CALCULATE_W(level) \
			(1.0/POWER(2,level))
#define AAF_DOM(_dom) ( \
			(struct AAF_dom *)(_dom)->sched_priv)
#define APPROX_VAL(val) (floor(val))
/* can freely access the global data pointer coming from struct scheduler */
#define AAF_PRIV_INFO(_domain) (\
				(struct AAF_private_info *)(_domain)->sched_data)
#define AAF_PCPU(_pc) \
		((struct aaf_pcpu *)per_cpu(schedule_data,_pc).sched_priv)

/* just for debugging purposes */
/*
int main()
{
printk("%d",PWR_TWO(3));
return ;
}
*/
/* The AAF's PCPU structure */
struct AAF_pcpu
{
struct timeslice *t_slices;
/* for future use, in case */
struct list_head lists;
    s_time_t hp;
};
/* The AAF's VCPU structure */
/* s_time_t in microsecond measurement */
struct AAF_vcpu
{
struct vcpu *vcpu;
struct AAF_dom *dom;
struct list_head element; /* vcpu, an element on the linked list. */

s_time_t comp_time_slice; /*computational time slice */
s_time_t deadline_abs;   /*absolute deadline */
s_time_t deadline_rel;   /*relative deadline */
unsigned flags; /* for future use */ 

};

/* The AAF's domain structure */
struct AAF_dom
{
/* AAF params */
unsigned int k;
double alpha;
struct list_head vcpu; /*link all the vcpu's inside this domain */
struct list_head element; /* linked list on aaf_private */
struct domain *dom; /* pointer to the superset domain */
/* each domain has a specific aaf value */
/* Hence, passing a FNCPTR of aaf_calc() to this struct */
double (*aaf_calc)(double alpha,int k);
/* level0, level 1,level 2...have some time slices in them,
* hence a 2d array */
int **level;
/* calculates the distance between time slices within */
int distance;
    s_time_t period;
    int distindex;
};

/* Scheduler Private data */
/* includes a global RUN-Q */

struct AAF_private_info
{
/* global lock for the scheduler */
spinlock_t lock;
int vcpu_count;
cpumask_t cpus; /* cpumask_t for all available physical CPUs */
};

/* the time slice of the system */
struct timeslice 
{
/* list_head for  iterating thru the list of timeslices */
struct list_head timeslices;
/* AAF_vcpus to get the sequence of time slices */
struct AAF_dom *doms;
};

/* alloc_pdata */
/* allocate before initialize */
/* for initializing the data structures inside a pcpu */
static void * AAF_init_pdata(const struct scheduler *ops, int cpu)
{
    unsigned long flags;
    /* docking global data ptr of struct scheduler to aaf_private_info struct */
    struct AAF_private_info *prv = AAF_PRIV_INFO(ops);
    /* protect the region between irq locks */
    spin_lock_irqsave(&prv->lock,flags);
    /* now turn on bit 'cpu' in mask */
    cpumask_set_cpu(cpu, &prv->cpus);
    spin_unlock_irqrestore(&prv->lock,flags);
    printk(KERN_ERR "Error in Initialization of PDATA");
}

/* alloc_pdata */
/* Allocates memory to the struct pcpu */
/* returns a struct of pcpus */
static void * AAF_alloc_pdata(const struct scheduler *ops, int cpu)
{
    struct AAF_pcpu *pcpus;
    pcpus = xzalloc(struct AAF_pcpu);
    INIT_LIST_HEAD(&pcpus->lists);
    return pcpus;
}

/* free_pdata */
/* frees the allocated data */
static void * AAF_free_pdata(const struct scheduler *ops,void *pcpu,int cpu)
{
    struct AAF_private_info *priv = AAF_PRIV_INFO(ops);
    /* PCPU either points to a valid struct AAF_pcpu or is NULL,xfree has
     * to be called after AAF_deinit_pdata */
    printk("CPU Deallocation: %d\n",cpu);
    xfree(pcpu);
}

/* deinit_pdata */
/* sequence of calling: alloc_pdata to be called before this and
 * free_pdata has to be called after this */
static void AAF_deinit_pdata(const struct scheduler *ops, void *pcpu, int cpu)
{
    struct AAF_pcpu *pc = pcpu;
    struct AAF_private_info *priv = AAF_PRIV_INFO(ops);
    /* Check if we are deinitializing the same pcpu and cpu bit set in the mask */
    ASSERT(pc && cpumask_test_cpu(cpu,priv->cpus);
    cpumask_clear_cpu(cpu, &priv->cpus);
}
/* Pick CPU: wisely select CPUs for a VCPU */
/* Valid CPU of a VCPU is an intersection of VCPU's affinity 
* and the available CPUs on the system */
/* Returns the PCPU number that is to be assigned */

static int aaf_cpu_pick(const struct scheduler *ops, struct vcpu *v)
{
cpumask_t cpus;
cpumask_t *online;
int cpu_no;
/* store in cpus variable the mask of online CPUS on which domain can run */
online = cpupool_scheduler_cpumask(v->domain->cpupool);
/* cpumask_and(destination,src1,src2)
* then do an intersection of online and available_pcpus*/ 
cpumask_and(&cpus,online,v->cpu_hard_affinity);
/* Try to find if there is a processor on which the vcpu is residing,
* else go ahead with finding available other PCPUs */
/*int cpumask_test_cpu(cpu,mask): true iff 'cpu' set in mask */ 
/* if not found on current vc->processor, then go for 
* Next CPU cycling from 'v->processor' */
cpu_no = cpumask_test_cpu(v->processor,&cpus)? v->processor:
						cpumask_cycle(v->processor,&cpus);
/*verify if mask is empty or set and check if cpu_no is set in mask */
ASSERT(!cpumask_empty(&cpus) && cpumask_test_cpu(cpu_no,&cpus));
/*If assert is successful, get the CPU_ID */
return cpu_no;
}

/********************* APPROXIMATION FUNCTION *******/
static double approx_val(double val)
{
    if(val - APPROX_VAL(val)>0.99999)
        return(APPROX_VAL(val)+1);
    if(val - APPROX_VAL(val)>0.49999 && val - APPROX_VAL(val)< 0.5)
        return (APPROX_VAL(val)+0.5);
    if(val - APPROX_VAL(val)> 0 && val - APPROX_VAL(val)<0.00001)
        return APPROX_VAL(val);
    return(val);
}

/*** HELPER FUNCTIONS ***/
/* GCD and LCM are for calculating Hyperperiod of the tasks */
static inline s_time_t GCD(s_time_t a, s_time_t b)
{
    if(b==0)
        return a;
    return GCD(b,a%b);
}


/* this includes the LCM of all the periods of the VCPUs
* within a given domain */
#ifndef __AAF_HP__
static inline s_time_t hyperperiod(struct AAF_vcpu *vcpus)
{
s_time_t hp = vcpus->period; /* to get initial instance first */
struct list_head *iterator;
struct AAF_vcpu *vcpus1;
/*list_for_each() is taking in position and head as arguments 
* to iterate through the list*/
/* list_entry is initializing the list that then enables us
* to use the elements of the struct that contains this list */
	list_for_each(iterator,&vcpus->element)/* iterates thru the list */
	{
	       vcpus1=list_entry(iterator,struct AAF_vcpu,element);
		hp=((vcpus1->period)*hp)/(GCD(vcpus1->period),hp);
	}
return hp;
}
#else
/* Takes a carrier ie: domain with a period
 * puts it to a destination PCPU and then re-
 * calculates Hyperperiod of that PCPU */
static inline s_time_t Hyperperiod(struct AAF_dom *domains, struct AAF_pcpu *pcpus)
{
    struct AAF_pcpu *pcpu_list;
    struct list_head iterator;
    /* Each domain has a period */
    pcpus->hp =domains->period;
    list_for_each(iterator,&pcpus->lists)
    {
        pcpu_list=list_entry(iterator,struct AAF_pcpu,lists);
        pcpu_list->hp = (((domains->period)*(pcpus->hp))/(GCD(domains->period),pcpus->hp));
    }
    return pcpu_list->hp;
}
#endif
/* math.h floor */
static int floor(double val)
{
int result = (int)val;
if(result>val && result<0)
	return (result-1);
else
	return result;
}

/* math.h ceil */
static int ceil(double val)
{
int result =(int)val;
if(result>val && result<0)
	return (result);
else
	return (result+1);
}

/* math.h log10 */
static double ln(double x)
{
double old_sum=0.0;
double number1 = (x-1)/(x+1);
double number_2 = number1*number1;
double denom = 1.0;
double frac = number1;
double term = frac;
double sum = term;

while(sum!= old_sum)
{
old_sum=sum;
denom+=2.0;
frac*=number_2;
sum+= frac/denom;
}
return 2.0*sum;
}

/* AAF calculation Function */
/* ****** Mem Alloc NOT DONE YET ******/
/* Assuming struct aaf_dom *doms is already 
* initialized and allocted in init_dom_data */
static double aaf_calc(doms->alpha,doms->k))
{
double result;
int number;
if(doms->alpha==0)
	return 0;
if(doms->alpha>0 && doms->k==1)
{
	number = floor(LOG10(doms->alpha)/LOG10(0.5));
	return (1/PWR_TWO(number));
}
else
{
	number= ceil(LOG10(doms->alpha)/LOG10(0.5));
	result=(1/PWR_TWO(number));
	return(aaf_calc((doms->alpha)-result),(doms->k)-1)+result);
}
}

/* finds Max AAF value of all the domains in the system */
static double findmaxAAF(struct AAF_dom *domains)
{
double result=0;
list_head *iteration;
 list_for_each(iteration,&domains->element)
{
	doms=list_entry(iterator,struct AAF_dom,element);
	if((domains->aaf_calc(domains->alpha,domains->k))>0 && (domains->aaf_calc(domains->alpha,domains->k) >result))
		result = domains->aaf_calc(domains->alpha,domains->k);
}
return result;
}

#ifndef __AAF_SINGLE__
static inline void aaf_single(struct AAF_dom *domains)
{
struct list_head *iterator;
struct timeslices slices;
struct AAF_vcpu *vcpu_s;
struct AAF_dom *doms;
doms->level=0;
/* spans across all the domains */
list_for_each(iterator,&domains->element)
{
	doms=list_entry(iterator,struct AAF_dom,element);
	while(!list_empty_careful(doms->element))
	{
		w = PWR_TWO(level);
		if((doms->aaf_calc(doms->alpha,doms->k))>w || 
					(doms->aaf_calc(doms->alpha,doms->k)==w))
			{
				/* storing timeslices to partition at level */
				vcpu_s->comp_time_slice=doms->level
				doms->aaf_calc(doms->alpha,doms->k)-=w;
				if(doms->aaf_calc(doms->alpha,doms->k)==0)
					/* if A_i==0 then deduct a partition */
					list_del(&doms->element);
			 }
		(doms->level)++;
}
}
}
#else
static inline void aaf_single(struct AAF_dom *doms)
{
    /* maxtsize is the index of a time slice */
int level=0,firstlevel, maxtsize;
double w=1,maxaaf;
maxaaf=findmaxaaf(doms);
struct list_head *iterator;
    struct AAF_pcpu *pcpus;
    struct AAF_dom *domains;
/* n is the number of partitions
* 2D array storing the time slices of each partitions */
    int number_of_domains = getCount(&doms->element);
    int t_p_counter[number_of_domains];
    int t_p [number_of_domains][maxtsize];
    int counter = number_of_domains;
    while(counter > 0)
    {
        int w = 1/PWR_TWO(level);
        int tsize = w * Hyperperiod(doms,pcpus);
        /* iterate through the list of domains to collect aaf's */
        for(int i=0;i<NUMBER_OF_DOMAINS;i++)
        {
            if(domains[i].aaf_calc(domains[i].alpha,domains[i].k)>w || domains[i].aaf_calc(domains[i].alpha,domains[i].k)==w)
            {
                for(int j=1;j<=tsize;j++)
                {
                    t_p[i][t_p_counter[i]++]= firstAvailableTimeSlice + (j-1)*(domains[i].distance);
                }
                /* updates the first available time slice */
                firstAvailableTimeSlice = findFirstAvailableTimeSlice();
                domains[i].aaf_calc(domains[i].alpha,domains[i].k) -=w;
                if(domains[i].aaf_calc(domains[i].alpha,domains[i].k)<=0.0001)
                    counter--;
                (domains[i].distindex)++;
                domains[i].distance *= domains[i].distindex;
            }
        }
        level++;
    }
    /* sorting */
    /* content inside t_p[][] is transferred to time slice linked list, sort the slices and then insert in an increasing order */
    for(int i=0;i<NUMBER_OF_DOMAINS;i++)
    {
        /* perform a merge sort on the content inside t_p[][] */
    }
#endif
 
/* Gets the Count of elements inside the Linked List */
/* can be optimized by adding it in the sched_Priv for aaf_dom_priv */
static int getCount(struct list_head *head)
{
    int count =0;
    struct list_head *current = head;
    while(current != NULL)
    {
        count++;
        current = current->next;
    }
    return count;
}


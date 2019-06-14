/****
* sched_aaf_rtp.c
*
*
*  Created by Pavan Kumar Paluri and Guangli Dai on 3/20/19.
****/
/*** Update as of Apr 25,2019 - AAF_single included, aaf_calc required for aaf_single has to be included into the code ***/

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/domain.h>
#include <xen/delay.h>
#include <xen/event.h>
#include <xen/time.h>
#include <xen/perfc.h>
#include <xen/sched-if.h>
#include <xen/softirq.h>
#include <asm/atomic.h>
#include <xen/errno.h>
#include <xen/trace.h>
#include <xen/cpu.h>
#include <xen/keyhandler.h>
#include <xen/trace.h>
#include <xen/double_test.h>
/* Design:
 
 * AAF - design : Per Domain - RUNQ
 
 */
#define RM 1
#define EDF 0
#define AAF_PRIV(_ops)   ((struct aaf_private *)((_ops)->sched_data))
#define AAF_PCPU(_cpu)   ((struct aaf_pcpu *)per_cpu(schedule_data, _cpu).sched_priv)
#define AAF_VCPU(_vcpu)  ((struct aaf_vcpu *)(_vcpu)->sched_priv) /* _vcpu takes in per_cpu(...) */
/* #define AAF_DOM(_dom)    ((struct aaf_dom *)(_dom)->sched_priv) 
/* #define RUNQ(_dom)       (&AAF_DOM(_dom)->runq) /* Each Domain has a RUNQ */
#define AAF_RUNQ(_dom)    (&(_dom)->runq)
#define VCPU_ON_RUNQ(_vcpu) !list_empty(&_vcpu->runq_elem) /* To validate if a given VCPU is inside RUNQ */
/*
 * Used to printout debug information
 */
#define printtime()     ( printk("Time in AAF Scheduler %d : %3ld.%3ld : %-19s \n", smp_processor_id(), NOW()/MILLISECS(1), NOW()%MILLISECS(1)/1000, __func__) )
#define HYPERPERIOD 1000
/* System wide private data
 * Global lock
 */
/*
 * aafdom: List of available domains */
struct aaf_private {
  struct list_head grunq;
  struct list_head *grun;   
 // spinlock_t lock;
    struct list_head aafdom;
    cpumask_t cpus;
    unsigned priority_scheme; /* EDF or RM */
    int dom_booted;/*set the dom_booted to 1 when a dom is booted*/
   // s_time_t hyperperiod;
   struct list_head pcpu_list;

};

struct aaf_dom{
    struct list_head runq; /* Per domain runQ */
    struct domain *dom;
    struct list_head vcpu; /* Link its VCPUS */
    struct list_head aafdom_elem; /* element inside the linked list in aaf_private */
};

struct aaf_vcpu {
    struct list_head runq_elem; /* element inside the runQ list */
    
    /* Upper links */
    struct aaf_dom *sdom;
    struct vcpu *vcpu;
    struct list_head vcpu_elem;
    /* Considering Deadline and Period to be the same (Implicit deadline) */
    s_time_t arrival_time; /* Multiple Arrival times, assume every task arrives at 0 for time being */
    s_time_t current_deadline; /* Current Deadline (For EDF) */
};

struct aaf_pcpu {
 struct list_head ts; /* Time slice listhead */
 struct list_head *list_now; /* current list_head */ 
 struct list_head pcpu_elem;
 struct list_head vcpu_head; // list head for vcpus
};

/* struct timeslice */
struct timeslice {
 struct list_head ts_elem; /* timeslice element inside the domain-timeslice list */
// s_time_t hyperperiod;
 struct aaf_dom *aafd; /* pointer to aaf_dom to maintain a key value pair within timslice struct */
  int index; /* index of a time slice */
   s_time_t run_time;
 };


/* list_entry: get the struct for this entry */
static struct aaf_vcpu * __runq_elem(struct list_head *elem)
{
    return list_entry(elem,struct aaf_vcpu, runq_elem);
}

/* list_entry: get the struct for this entry */
static struct timeslice *__get_ts_elem(struct list_head *elem)
{
   return list_entry(elem,struct timeslice, ts_elem);
}


/* Typecast domain's sched_priv to struct aaf_dom */
static struct aaf_dom *_get__aaf_dom(struct domain *dom)
{
  return dom->sched_priv;
}

/* This function deletes a given VCPU from list */
static inline void __runq_remove(struct aaf_vcpu *avc)
{
    if(VCPU_ON_RUNQ(avc))
        list_del_init(&avc->runq_elem);
}

/****** AAF INCLUSION FOR DOMAIN SCHEDULING *****/
/* Heap sorting -- for sorting of timeslices and inclusion into timeslice table */
static inline void swap(int *a, int *b)
{
  int temp = *a;
  *a = *b;
  *b = temp;
}


static inline void down_adjust(int *arr, int i, int n)
{
   int son = i*2+1, parent =i;
  while(son < n)
   {
     if(son+1<n && arr[son+1] > arr[son])
          son++;
    if(arr[parent] > arr[son])
	return;
    swap(&arr[parent], &arr[son]);
    parent = son;
   son = parent*2+1;
   }
}

static inline void heap_sort_insert(int *arr, int n, int pcpu,struct aaf_dom *dom)
{
  int counter=0;
  struct aaf_pcpu *apcpu = AAF_PCPU(pcpu);
  struct timeslice *t;
  struct list_head *head = &apcpu->ts, *end =(head)->prev, *temp=end;
  // init the heap 
  for(counter = n/2-1; counter >=0; counter--)
  {
    down_adjust(arr, counter,n);
   }
 for(counter = n-1; counter>0; counter--)
 {
    /*arr[0] is now the largest time slice, insert it from the back
         * Utilize functions list_last_entry and list_prev_entry to accomplish the insertion.
         * Mind that the index of the time slice needs to be mentioned in struct timeslice.
        */
   while(temp!=head && list_entry(temp,struct timeslice, ts_elem)->index > arr[counter])
    {
       temp = temp->prev;
    }
   t->aafd = dom;
   t->index = arr[counter];
   list_add(&t->ts_elem,temp);
   swap(&arr[0],&arr[counter]);
   down_adjust(arr,0,counter-1);
 }
}

/* VCPU insert into a RUNQ *
 * runQ_insert(scheduler, domain to be passed, vcpu to be inserted) *
 * as opposed to runq_insert in rt_partition where each RUNQ is specific to a CPU*/
static void __runq_insert(const struct scheduler *ops, struct aaf_dom *dom, struct aaf_vcpu *avc)
{
    struct list_head *runq = AAF_RUNQ(dom);
    struct list_head *iter;
    struct aaf_private *prv = AAF_PRIV(ops);
        
    /* If the domain under consideration is Dom0, then exit the insertion of VCPUs as there is no RUNQ in Dom0 */
   // if(dom->dom->domain_id ==0)
       // return;
    /* Error check to see if given vcpu is on the RUNQ */
    /* If the given vcpu already on the RUNQ, return from this function */
    if(VCPU_ON_RUNQ(avc))
        return;
    /* We also while iterating through this list of vcpus in a domain,
     * it will be helpful to sort the runq based on EDF while this iteration happens */
    list_for_each(iter,runq)
    {
      if(iter==runq)
          continue; 
       struct aaf_vcpu * iter_vcpu = __runq_elem(iter);
        
        /* EDF sorting */
        /* Compare the incoming VCPU's deadline with the existing VCPU's already inside the RUNQ */
        if((prv->priority_scheme == EDF) &&
           (avc->current_deadline<= iter_vcpu->current_deadline))
        {
		//BUG(); // control check @ line 141
            break;
        }
        /* If the incoming vcpu's deadline > vcpu deadlines of all the existing ones in the q, then put this vcpu at the end of the list */
    }
    /* If this vcpu has the largest deadline, finish the iteration of the RUNQ and insert it at the tail */
    list_add_tail(&avc->runq_elem, iter);
   // __list_add_rcu(&avc->runq_elem, iter->prev,iter->next);
}


/* Dump a Dummy VCPU / Print VCPU info and stats */
/*
static void aaf_dump_vcpu(struct aaf_vcpu *vc)
{
    if(vc==NULL)
    {
        printk("NULL");
        return ;
    }
    
    printk("[%5d.%-2d] cpu %d, (%-2d, %-2d), cur_b=%ld cur_d=%lu last_start=%lu onR=%d runnable=%d\n",
           vc->vcpu->domain->domain_id,
           vc->vcpu->vcpu_id,
           vc->vcpu->processor,
           vc->current_deadline,
           vc->arrival_time,
           VCPU_ON_RUNQ(vc));
}
*/
/* Prints both domain and vcpus inside that domain info */
static void aaf_print_info(struct scheduler *ops)
{
  struct list_head *iter_dom,*iter;
  struct aaf_private *prv = AAF_PRIV(ops);
  struct aaf_vcpu *vc;
  struct list_head *runq;
  

  printk("Domain Info");
  /* Iterate through the list that has head in AAF_PRIV */
  list_for_each(iter_dom, &prv->aafdom)
  {
    struct aaf_dom *dom;
    /* Now get the struct for this entry */
   dom = list_entry(iter_dom, struct aaf_dom, aafdom_elem);
   printk("\t Domain Id: %d\n",dom->dom->domain_id);
   runq=RUNQ(dom->dom);
   /* Access to the RUNQ inside each domain is available now */
   list_for_each(iter,runq)
   {
      struct aaf_vcpu *iter_vcpu = __runq_elem(iter);
      /* Call function dump_vcpu to print vcpu information */
      //  aaf_dump_vcpu(iter_vcpu);

    }
   }
   printk("\n");
}

/* Initialization segment */
static int aaf_init(struct scheduler *ops)
{
 struct aaf_private *priv;
 priv = xmalloc(struct aaf_private);
 if(priv == NULL)
 {
	printk("Failed at aaf_private memory allocation\n");
	return -ENOMEM;
 }
 memset(priv,0,sizeof(struct aaf_private)); /* Default set priv mem to 0 */
 ops->sched_data = priv;
 /* Grab a lock before dealing with the parameters within of struct private */
   // spin_lock_init(&priv->lock);
 INIT_LIST_HEAD(&priv->aafdom);
 INIT_LIST_HEAD(&priv->grunq);
 INIT_LIST_HEAD(&priv->pcpu_list);
 priv->grun = &priv->grunq;
 cpumask_clear(&priv->cpus);
 priv->priority_scheme = EDF; /* Let EDF be default priority scheme */
 priv->dom_booted = 0;
 printk("\n");
 printk("Running using EDF scheduler\n");
 return 0;
 }

/* Deinit segment - free the data assigned to sched_data field of struct scheduler */
/* We need to make sure we are exactly freeing the data that was intialized to, so use const struct to ensure content
 * in struct scheduler *ops : ops->sched_data does not change while freeing */
static void aaf_deinit(const struct scheduler *ops)
{
 struct aaf_private *prv;
 /* Print_time fn to be added */
 printk("\n");
 prv = AAF_PRIV(ops);
 if(prv!=NULL)
 {
  xfree(prv);
 }
}


 inline void run_length(int cpu)
{

 struct aaf_pcpu *pc = AAF_PCPU(cpu);
 /* list_head ts: time slice list_head (in aaf_pcpu)
  * list_head ts_elem : list_head inside timeslice */
 struct list_head *pos;
 /* iterate through timeslice-domain table and fill all the 1000 entries with domain0 to run */
 list_for_each(pos,&pc->ts)
 {
    struct timeslice *ts_element = __get_ts_elem(pos);
    //ts_element->hyperperiod = 1000;
   /* Attempt: To fill all the corresponding entries of timeslices with 30 ms */
  // ts_element ->run_time = MILLISECS(30);
  //  ts_element->aafd->dom->domain_id =0;
 } 
}


/* Initializa a PCPU */
/* Maintains a per-cpu spinlock pointing to the global spinlock */
static struct aaf_pcpu *aaf_alloc_pdata(const struct scheduler *ops, int cpu)
{
   struct aaf_private *prv = AAF_PRIV(ops);
   struct aaf_pcpu *pc;
   struct timeslice *tslices;
   unsigned long flags;
   pc = xzalloc(struct aaf_pcpu);
   if( pc==NULL)
	return NULL;
  //tslices = xmalloc(10*sizeof(struct timeslice));
  // allocation of 1000 entries for domain-timeslice table 

  /*
  if(tslices == NULL)
    { 
      printk("%s, xmalloc failed\n",__func__);
      BUG();
      return NULL;
    }
  */
  //memset(tslices,0,10*sizeof(*tslices));
   /* spinlock per-cpu activation */
   //spin_lock_irqsave(&prv->lock, flags);

   cpumask_set_cpu(cpu,&prv->cpus);
   INIT_LIST_HEAD(&pc->ts); 
   INIT_LIST_HEAD(&pc->pcpu_elem);
  INIT_LIST_HEAD(&pc->vcpu_head);
  pc->list_now = &pc->vcpu_head; // current pointer to vcpu_list
  // Add each pcpu to the list of PCPUs maintained in aaf_private
   list_add_tail(&pc->pcpu_elem,&(prv->pcpu_list));
    for(int i=0;i<10;i++)
   {
	struct timeslice *tptr = xmalloc(struct timeslice);
	 tptr->run_time = MILLISECS(30);
	tptr->aafd = NULL;
   	INIT_LIST_HEAD(&tptr->ts_elem);
	list_add_tail(&tptr->ts_elem, &pc->ts);
   }
   if( per_cpu(schedule_data, cpu).sched_priv == NULL)
	per_cpu(schedule_data,cpu).sched_priv = pc;
   //spin_unlock_irqrestore(&prv->lock,flags);
   printtime();
 //  run_length(smp_processor_id());
   printk("total CPUs: %d",cpumask_weight(&prv->cpus));
   return  pc;
}
#ifdef AAF_SUPERNOVA
static inline void AAF_single(const struct scheduler *ops)
{
  struct aaf_dom *dom;
  struct aaf_private *prv;
  double maxaaf;
  int w =1;
  s_time_t firstavailabletimeslice=0;
  db temp,temp1;
  int level=0,counter,i=0,hyperp;
  hyperp = prv->hyperperiod;
  s_time_t t_p_counter[sizeof(s_time_t)];
  s_time_t distance[sizeof(s_time_t)];
  s_time_t t_p[sizeof(s_time_t)][sizeof(s_time_t)];
  db p;
  p.x =1;
  temp.x=1;
  temp.y =10000;
  /* Hard coded regularity for Domain 0 */
  dom->k =1;
  int num =1;
  int denom =1;
   /* Iterate through list of domains */
   list_for_each_entry(dom,&AAF_PRIV(ops)->aafdom, aafdom_elem)
   {
      counter++; //calculating number of domains
   }


  	while(counter>0)
	{

				p.y = TWO_PWR(level);
				w = (int) (p.x/p.y); /* Forced conversion into int */
				int tsize = w*(hyperp);
		/* prv->hp after iterating through all the domains in the environment has a unique and
	 	 * a final hyperperiod for the entire system now */

                   /* iterate through the list of domains */
		list_for_each_entry(dom,&AAF_PRIV(ops)->aafdom,aafdom_elem)
		{
			int l;
		//	int num = (int)(dom->aaf_calc(dom->alpha,dom->k).x);
		//	int denom =  (int)(dom->aaf_calc(dom->alpha,dom->k).y);
			if((int)(num/denom)>=w)
			{
				for(int j=1;j<=tsize;j++)
				{
				/* firstAvailableTimeSlice ==0 for time being */
				 t_p[l][t_p_counter[l]++] = 0+(j-1)*(distance[counter]);
				}
			/*firstAvailableTimeSlice=findfirstAvailableTimeSlice();*/
			int x= (int)num/denom; 
				x-=w;
			temp1= dom->aaf_calc(dom->alpha,dom->k);
			if(less_than(&temp1,&temp)==0) 
				counter--;
		}
			l++;
		}
		level++;
	}
	i=0;
	list_for_each_entry(dom, &AAF_PRIV(ops)->aafdom, aafdom_elem)
	{
		heap_sort_insert(t_p[i],t_p_counter[i], smp_processor_id(),dom);
		i++;
	
	}

 }
#else
#endif
/* Deinit PCPU */
static void aaf_free_pdata(const struct scheduler *ops, void *pcpu, int cpu)
{
 struct aaf_private *prv = AAF_PRIV(ops);
 struct aaf_pcpu *pc = pcpu;
 //struct timeslice *ts;
 cpumask_clear_cpu(cpu, &prv->cpus);
 printtime();
 printk("cpu=%d\n",cpu);
 xfree(pc);
}

/* Pick a PCPU to run */
static int aaf_cpu_pick(const struct scheduler *ops, struct vcpu *vc)
{
  cpumask_t ccpu;
  int cpu;
  struct aaf_private *prv = AAF_PRIV(ops);
  cpumask_copy(&ccpu,vc->cpu_hard_affinity);
  cpumask_and(&ccpu,&prv->cpus,&ccpu);
  cpu = cpumask_test_cpu(vc->processor, &ccpu)
                        ? vc->processor
                        : cpumask_cycle(vc->processor, &ccpu);
   printk("\nPicking up CPU: %d\n",cpu);
        ASSERT(!cpumask_empty(&ccpu) && cpumask_test_cpu(cpu, &ccpu));
  return cpu;
}

static inline void __vcpu_deassign(struct aaf_private *prv, struct vcpu *v, unsigned int cpu)
{
 struct aaf_vcpu *avc = AAF_VCPU(v);
 list_del_init(&avc->vcpu_elem);
}


/* to remove a vcpu */
static void aaf_vcpu_remove(const struct scheduler *ops, struct vcpu *vc)
{
  unsigned int cpu = vc->processor;
   struct aaf_vcpu * const avc = AAF_VCPU(vc); /*storing it in the sched_data field */
   struct aaf_dom * const adom = avc->sdom;

   printtime();
   __vcpu_deassign(ops, vc, cpu);
 // aaf_dump_vcpu(avc);
}


static inline void __vcpu_assign(struct aaf_private *priv, struct aaf_vcpu *vc, unsigned int cpu)
{
  struct vcpu *v = vc->vcpu;
  struct aaf_pcpu *p = AAF_PCPU(cpu);
  
 list_add_tail(&vc->vcpu_elem, &p->vcpu_head); // adding a vcpu to the list of vcpus in pcpu
 v->processor = cpu;
if( unlikely(tb_init_done) )
{
 struct{
 uint16_t vcpu, dom;
 uint32_t cpu;
 } d;

 d.dom = v->domain->domain_id;
 d.vcpu = v->vcpu_id;
 d.cpu = cpu;
}

}



/* Insertions of vcpu into Domian's RUNQ unlike PCPU's RUNQ in other schedulers */
static void aaf_vcpu_insert(const struct scheduler *ops, struct vcpu *c)
{
  unsigned int cpu;
  struct aaf_private *priv = AAF_PRIV(ops);
  struct aaf_vcpu *vc = c->sched_priv;
  struct aaf_dom *adom = vc->sdom;
  ASSERT(!is_idle_vcpu(c)); 
  printtime();
   // VCPU insert code has to be inserted here 
  printk("aaf_vcpu_insert invoked on VCPU# %d\n",c->vcpu_id);
   cpu = c->processor = aaf_cpu_pick(ops, c);
    __vcpu_assign(priv, vc, cpu);
}


/* Init  all the domains of the scheduler (Dom Us) */
static void * aaf_dom_alloc(const struct scheduler *ops, struct domain *dom)
{
 /*  BUG_ON(1); */
 printk("\n Inside alloc_domdata\n");
 struct aaf_dom *aafd;
 // struct aaf_pcpu *pc;
  unsigned long flags;
  struct aaf_private *priv = AAF_PRIV(ops);
  struct list_head *iter; 
  priv->dom_booted = 1;
  printtime(); 
  printk("DomID: %d\n",dom->domain_id);
  /* Allocate domain data with domai's info brought in as an argument */
  /*  tslices = xzalloc(struct timeslice);
         return NULL;
   */
  aafd = xzalloc(struct aaf_dom);

  if(aafd == NULL)
  {
    printk("Error on Domain 0 xzalloc\n");
    return (*(int*)(-ENOMEM)); // return No Mem status
  }
  printk("Control reached IniT_list_head in alloc_dom\n");
   // dom->sched_priv = aafd; 
  INIT_LIST_HEAD(&aafd->runq);
  INIT_LIST_HEAD(&aafd->vcpu);
  INIT_LIST_HEAD(&aafd->aafdom_elem); 
  aafd->dom = dom;
  // Get the first pcpu from the list of pcpus maintained in aaf_private 
  struct aaf_pcpu *pc = list_first_entry(&priv->pcpu_list,struct aaf_pcpu, pcpu_elem);
  // Now we have pcpu * with us, based on this we can iterate through timeslice table and assign Dom0 
   list_for_each(iter,&pc->ts)
    {
	struct timeslice *tss = list_entry(iter,struct timeslice,ts_elem);
         tss->aafd = aafd;
     }

  printk("Control leaving alloc_domdat\n");
  return aafd;
}


static void aaf_dom_destroy(const struct scheduler *ops, void *data)
{
  struct aaf_dom *adom=data;
  struct aaf_private *prv = AAF_PRIV(ops);
  if(adom)
  {
	unsigned long flags;
	//spin_lock_irqsave(&prv->lock,flags);
	list_del_init(&adom->aafdom_elem);
	//spin_unlock_irqrestore(&prv->lock,flags);
	xfree(adom);

   }
 }

/* VCPU memory allocations section */
static void *aaf_alloc_vdata(const struct scheduler *ops,struct vcpu *v,void *data)
{
   printk("Control entering alloc_vdata\n");
	struct aaf_vcpu *avc;
       struct aaf_private *prv=AAF_PRIV(ops);
	avc = xzalloc(struct aaf_vcpu);
	printtime();
	if(avc == NULL)
		return NULL;
       printk("Control checked for NULL: is successful in alloc_vdata\n");
	INIT_LIST_HEAD(&avc->runq_elem);
	INIT_LIST_HEAD(&avc->vcpu_elem);
	avc->vcpu = v;
	if(data == NULL)
	{
	    list_add_tail(&avc->runq_elem,&prv->grunq);
        }
       else
       {
         avc->sdom = data;
       }
      return avc;
}

/* VCPU freeing of memory */
static void aaf_free_data(const struct scheduler *ops, void *priv)
{
	struct aaf_vcpu *avc = priv;
	xfree(avc);
}



static struct task_slice aaf_schedule(const struct scheduler *ops, s_time_t now, bool_t tasklet_work_scheduled)
{
   int cpu_number=-1;
  printk("Entering AAF schedule\n"); /* printing time in do_schedule */
   struct task_slice ret;
   struct aaf_vcpu *vc;
   struct list_head *pos,*pos_dom;
   cpumask_t cpus;
   struct domain *dom;
   // BUG();
 printk("inits done in aaf_schedule\n");
   struct aaf_private *priv = AAF_PRIV(ops);
 //  BUG();
  // ret.time = MILLISECS(10);
   /* choosing a cpu on the fly */ 
  
  const int cpu = smp_processor_id();
  
struct aaf_pcpu *pc = AAF_PCPU(cpu);
 printk("Cpu id in do_schedule is:%d\n",cpu);
  int flag = 0;  
 //  BUG();
  /* As vcpus are created and assigned even before a domain is created, we put this into a global runq */
 ret.time = MILLISECS(10);

  if(priv->dom_booted==0 && (!list_empty(&priv->grunq)))
   {

     
     // Spin Lock failure happening, Assertion FAILURE at vcpu_runstate_change @ 194 in schedule.c 
    //  BUG(); 
	list_for_each(pos,priv->grun->next)
	{
		if(pos==&priv->grunq)
		 	continue;
		vc = list_entry(pos, struct aaf_vcpu, runq_elem);
		cpumask_copy(&cpus, vc->vcpu->cpu_hard_affinity);
		cpu_number = cpumask_test_cpu(vc->vcpu->processor, &cpus)? vc->vcpu->processor: 
									cpumask_cycle(vc->vcpu->processor, &cpus);
	
		if(cpu!=cpu_number)
			continue;
		if(vc->vcpu->runstate.state == RUNSTATE_running || !vcpu_runnable(vc->vcpu))
			continue;	
		ret.task = &vc->vcpu;
		ret.time = MILLISECS(30);
                 priv->grun = vc;
		return ret;
	
	} 
	ret.time = -1;
        ret.task = idle_vcpu[cpu];
	// ret.task = NULL;
	return ret;
    }

  /* iterate through timeslice-domain table residing in the current cpu and collect the domain thru which vcpus inside 
   that domain could be accessed. */
/*
 else
 {
     // Before proceeding with extraction of timeslice-domain entry, we better check for CPU's since we have pointed Domain * to Domain0 in only a single PCPU


	// pc->ts is the list_head
	// Use it to get the first list_entry of timeslice-domain entry
	struct timeslice *ts = list_first_entry(&pc->ts,struct timeslice,ts_elem);

	if(ts->aafd==NULL) // since no allocation done to this PCPU, its better to check only the first entry and write it off...
	  ret.time = MILLISECS(30);
    
    else if(ts->aafd!=NULL) //start retreiving the vcpus of each domains now for this first entry
    // Now since we know its the pCPU that has Dom0 filled up values .. we can iterate thru this table of timeslice-domains..
    {
      struct timeslice *tm = __get_ts_elem(pos);
      struct aaf_dom *domn = tm->aafd;

      struct list_head *runq_x  = &(domn->runq);
      //  if(pos==&pc->ts)
               // continue;
      list_for_each(pos_dom,runq_x) // Now with that domain, we access VCPUS inside 
        {
          if(pos_dom==runq_x)
                continue; 
           struct aaf_vcpu *iter_vcpu = __runq_elem(pos_dom);
            if(vcpu_runnable(iter_vcpu->vcpu))
            {
                //      BUG(); //Bug at line 471
                    ret.task = iter_vcpu->vcpu;
                    flag = 1;
                   break;
            }

    	} // iterating thru vcpus of first dom ends..
     return ret;
   }


}
*/

 //  if(flag==0)
  //	ret.time = -1;
  //   BUG(); // Bug at line 482



 else
 {
     // Before proceeding with extraction of timeslice-domain entry, we better check for CPU's since we have pointed Domain * to D$


        // pc->ts is the list_head
        // Use it to get the first list_entry of timeslice-domain entry
        struct timeslice *ts = list_first_entry(&pc->ts,struct timeslice,ts_elem);

        if(ts->aafd==NULL) // since no allocation done to this PCPU, its better to check only the first entry and write it off...
          ret.time = MILLISECS(30);
    

	else if(ts->aafd!=NULL)
	{
	struct timeslice *tm = __get_ts_elem(pos);
	  struct aaf_dom *domn = tm->aafd;

      struct list_head *runq_x  = &(domn->runq);
      //  if(pos==&pc->ts)
      	//conitnue
      list_for_each(pos_dom,runq_x) // Access vcpus inside this domain!
      {
      // However, this domain does not have any vcpus linked to it until now.
      // So link the VCPUS in the global RUNQ maintained by aaf_private to DOm0
      if(pos_dom==runq_x) // if list_head, continue
      	continue;

    //   AAF_VCPU(domn->vcpu) = vc;
       struct aaf_vcpu *iter_vcpu = __runq_elem(pos_dom);

       if(vcpu_runnable(iter_vcpu->vcpu))
       {
       		// ret.time = MILLISECS(30);
       		ret.task = &iter_vcpu->vcpu;
       		flag =1;
       		break;
       }
   }
}
  return ret;
}

}
/* struct aaf_private _aaf_priv; */


static struct task_slice aaf_schedule1(const struct scheduler *ops, s_time_t now, bool_t tasklet_work_scheduled)
{
  struct task_slice ret;
  struct list_head *pos;
  struct aaf_private *priv = AAF_PRIV(ops);
  struct aaf_vcpu *avc = NULL;
 ret.time = MILLISECS(10);
 const int cpu = smp_processor_id();
 struct aaf_pcpu *pc = AAF_PCPU(cpu);

 list_for_each(pos, pc->list_now)
  {
    if(pos == &pc->vcpu_head)
	{
           continue;
	}
   avc = list_entry(pos, struct aaf_vcpu, vcpu_elem);
  if(vcpu_runnable(avc->vcpu))
   {
	ret.task = avc->vcpu;
        pc->list_now = &avc->vcpu_elem;
         break;
    }
  }
 return ret;
}

const struct scheduler sched_aaf_def = {
    .name = "AAF Single Scheduler",
    .opt_name = "aaf",
    .sched_id = XEN_SCHEDULER_AAF,
    .sched_data = NULL,
   /*  .dump_cpu_state = rtpartition_dump_pcpu,  */
 /* .dump_settings  = aaf_dump, */
    .init           = aaf_init,
    .deinit         = aaf_deinit,
    .alloc_pdata    = aaf_alloc_pdata,
    .free_pdata     = aaf_free_pdata,
    .alloc_domdata  = aaf_dom_alloc,
    .free_domdata   = aaf_dom_destroy,
  /*  .alloc_domdata  = aaf_alloc_domdata,
    .free_domdata   = aaf_free_domdata,
    .destroy_domain = rtpartition_dom_destroy,
    .alloc_vdata    = aaf_alloc_vdata,
    .free_vdata     = aaf_free_vdata,
*/

    .insert_vcpu    = aaf_vcpu_insert,
    .remove_vcpu    = aaf_vcpu_remove,
    .alloc_vdata    = aaf_alloc_vdata,
    .free_vdata     = aaf_free_data,
    .adjust         = NULL,

    .pick_cpu       = aaf_cpu_pick,
    .do_schedule    = aaf_schedule1,
    .sleep          = NULL,
    .wake           = NULL,
    
    .context_saved  = NULL,
    .yield          = NULL,
    .migrate        = NULL,
};

REGISTER_SCHEDULER(sched_aaf_def);


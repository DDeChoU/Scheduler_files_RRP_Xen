/************** AAF ******************
* Developers : Pavan Kumar Paluri & Guangli Dai
* AAF: Algorithm that creates fixed partitions and assisgns them
* to PCPUs based on the concept of levels and AAF
* Latest Modification : 1/15/2019
* Most Recent Update: Updated VCPU Schedule list functions, To return a Domain Handle and VCPU ID of a given VCPU, added init
* ,alloc,free,deinit PCPUs 
* Added alloc_dom_data that includes initialization and memory allocations of all the elements for 
* bot domains and partitions.
* free_domdata has been added to the set of helper functions that also includes the removal of 
* partitions along with the existing domains
* Also included a safe version of aaf_calc that calculates all AAF formulas for a range of availability factors and supply regularities
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
#include <xen/double_test.h>
/* Macros for debugging purposes */

#ifndef CHECK(_p)

#define CHECK(_p) \
do{ \
if(!_p) printk("Checking of '%s' failed at line %d of file %s\n", \
#_p,__LINE__,__FILE__);	\
} while(0) 

#else
#define CHECK(_p) (void 0)
#endif

/* Macros for Binary Operations */
#define EQUAL(a,b) ((!!a)==(!!b))
#define IMPLY(a,b) ((!a)||(b))
#define PWR_TWO(a) (1<< (a))
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
#define TIMESLICERRAYSIZE (65536)
#define AAF_COMP_MIN (MICROSECS(5))
#define AAF_MAX_DOMAINS_PER_SCHEDULE 12
/* can freely access the global data pointer coming from struct scheduler */
/* Hence typecasting it to type struct AAF_private_info */
#define AAF_PRIV_INFO(_d) (\
				(struct AAF_private_info *)(_d)->sched_data)
#define AAF_PCPU(_pc) \
		((struct aaf_pcpu *)per_cpu(schedule_data,_pc).sched_priv)
#define AAF_DOM(_dom) ( \
			(struct AAF_dom *)(_dom)->sched_priv)

/* To get the idle VCPU for a given PCPU */
#define IDLE(cpu) (idle_vcpu[cpu])

/****************************Definition of Structures**************************************/


/* Domain Handle Struct */



  /* Holds single entry of the AAF-RRP schedule */
typedef struct sched_entry_s
{
	/*
	 * Dom_Handle: Holds the UUID(handle) for the domain that this shed list refers to 
	 * VCPU_ID: Holds the number of the VCPU that this sched Entry refers to 
	 * runtime: Has the number of nanoseconds a VCPU has to run for this sched_entry
	 */

	xen_domain_handle_t dom_handle;
	int vcpu_id;
	s_time_t runtime;
	struct vcpu *vc;
} sched_entry_t;


struct AAF_private_info
{
	/*
	* Variables:
	* lock:		Spinlock for the scheduler.
	* cpus:			cpumask_t for all available physical CPUs.
	* ndom:			A linked list that track how many domains are under the control of this scheduler.
	* schedule[]:   This 1D Array holds the active AAF scheudle list. 
	*		when the system tries to start a VCPU, this schedule list is scanned to check for a matching 
			(handle,VCPU#) pair. IF the handle (UUID) and VCPU# match, then this VCPU is allowed to run.
	* num_sched_entries:  Returns the number of entries that are valid in the AAF scheduler, Do not confuse it with the 
	*                     number of domians or number of vcpus within a certain domain. A domain/mul VCPUs per domain can be
				 listed severl times 

	*		      whenever this scheduler is called
	* par_count:	Partition count, not used yet.
	*/

    spinlock_t lock;
    cpumask_t cpus;
    struct list_head ndom;
    struct list_head vcpu_list;
    sched_entry_t schedule[AAF_MAX_DOMAINS_PER_SCHEDULE];
    unsigned int num_sched_entries;
    int par_count;
};

struct AAF_pcpu
{
	/*
	* Variables:
	* time_list: 	The list head of the linked list of time slices. The linked list is the time-partition table.
	*					The items in the linked list contains a pointer to AAF_partition.
	* hp: 		 	The hyper-period of this pcpu. A hyper-period can accomodate the period of all vcpus. The unit is in microsecond
	* time_now:  	A pointer indicating where the system is in the time-partition table.

	*/

	struct list_head time_list;
	s_time_t hp;
	struct list_head *time_now; /*pointer to the time_list, so no INIT_LIST_HEAD for this */
};


struct AAF_vcpu
{
	/*
	* Variables:
	* vcpu: 		The pointer to the vcpu.
	* dom:  		The pointer to the domain.
	* vcpu_list:	An element that binds it to the linked list, the head of which is in AAF_partition.
	*
	* deadline_abs: Absolute deadline.
	* deadline_rel: Relative deadline.
	* flags:		For future usage.
	*/

    struct vcpu *vcpu;
    struct AAF_dom *dom;
    struct list_head vcpu_list; 

    s_time_t deadline_abs;   /*absolute deadline */
    s_time_t deadline_rel;   /*relative deadline */
    s_time_t period;
    unsigned flags;
};

struct AAF_dom
{
	/*
	* Variables:
	* dom:			The pointer to the domain.
	* inter_par:	The version now assumes each domain has only one partition. 
	* dom_list:		An element that binds it to the linked list, the head of which is in AAF_private.
	* dom_lock:		Spinlock for each domain.
	*/


    struct domain *dom;
    struct AAF_partition *inter_par;
    struct list_head dom_list;
    spinlock_t dom_lock;
	int k;
};



struct AAF_partition
{
	/*
	* Variables:
	* par_id:		The partition id.
	* vcpu_list:	The head of the linked list holding vcpus.
	* vcpu_now:		The pointer showing where the system is in the linked list.
	* ava:			Availability factor of the partition.
	* reg:			Regularity of the partition.
	* par_lock:		Spinlock for the partition.
	* aaf_calc:		Every partition has its own AAF value that is unique to every partition 
	*/
 
	unsigned int par_id;
	struct list_head vcpu_list;
	struct list_head *vcpu_now;
	/* Alpha re-introduced as an int(x)/int(y) parameter */
	s_time_t time_slices; /* These parameters come from user end */
	s_time_t period;
	
	db *alpha;
	int k;
	spinlock_t par_lock;
	db * (*aaf_calc)(db *alpha,int k); /*Moved to AAF_partition*/
};

struct time_slice
{
	/*
	* Variables:
	* time_list:	An element that binds it to the linked list, the head of which is in AAF_pcpu.
	* index:		The index of this time slice.
	* par_ptr:		The pointer of the partition this time slice will be served for.
	*/
	struct list_head time_list;
	int index;
	struct AAF_partition *par_ptr;
};


/******************************************************************************************/

/****************************Assistance Functions**************************************/

/********************** FUNCTION TO COMPARE 2 GIVEN DOMAINS **************************

	* h1: Pointer to Domain Handle 1
	* h2: Pointer to Domain Handle 2
	*********** RETURN VALS *********
	* <0: Handle 1 is less than handle 2
	* =0: Handle1 == handle 2
	* >0: Handle 1 > handle 2 */
static int dom_handle_cmp(const xen_domain_handle_t h1, const xen_domain_handle_t h2)
{
	return memcmp(h1,h2, sizeof(xen_domain_handle_t));
}
/***************************** DOM CMP ENDS ***************************************/

/************* VCPU LIST SEARCH ***************
	* ops: Pointerr to the instance of the struct scheduler
	 * handle: An instance of the Xen_domain_handle
	 * VCPU_ID: vcpu's ID 

	****** RETURN VAL ****
	* Pointer to the matching VCPU if there exists one
	* NULL Otherwise   */

static struct vcpu *find_vcpu(const struct scheduler *ops,xen_domain_handle_t handle,int vcpu_id)
{
	struct AAF_vcpu *vcp;
	/* Now loop thru the vcpu_list to look for the specified VCPU */
	list_for_each_entry(vcp, &AAF_PRIV_INFO(ops)->vcpu_list, vcpu_list)
		if( (dom_handle_cmp(vcp->vcpu->domain->handle, handle) == 0)
			&& (vcpu_id == vcp->vcpu->vcpu_id))
			return vcp->vcpu;
	return NULL;
}
/******************* VCPU LIST SEARCH ENDS ******************/

/******************** UPDATING XEN VCPU *******************
	*ops : pointer to the struct scheduler
	
	***** Return ****
	* None */
static void update_sched_vcpu(const struct scheduler *ops)
{
	unsigned int i, n_entries;
	n_entries = AAF_PRIV_INFO(ops)->num_sched_entries;
	for(i=0;i< n_entries;i++)
	{
		 AAF_PRIV_INFO(ops)->schedule[i].vc = find_vcpu(ops,AAF_PRIV_INFO(ops)->schedule[i].dom_handle,
								AAF_PRIV_INFO(ops)->schedule[i].vcpu_id);
	}
}

static s_time_t hyperperiod(struct AAF_partition *par)
{
	
	struct AAF_pcpu *pc;
	s_time_t temp_res_num,temp_res_denom;
	if(pc->hp==0)
	{
		pc->hp = par->alpha->y;
	}
	else
	{
		/* Numerator of the LCM stored in temp_res */
		temp_res_num = (par->alpha->y)*(pc->hp);
		temp_res_denom = (gcd(par->alpha->y,pc->hp));
		pc->hp = (s_time_t)(temp_res_num/temp_res_denom);

		/*pc->hp = (par->alpha.period)*(pc->hp)/GCD((par->period),pc->hp);*/
	}
	return (pc->hp);

	/* provides us with the index of the time slice corresponding to a partition */
	/*ts->index*/ 
}

/******************** AAF CALC FUNCTION ********************
	* struct alpha
	* int k: Supply regularity 
      ****** Return value ******
	* returns a struct of type double */
db *aaf_calc(db *factor,int k)
{
	db *result,*result1,*result2,*result3,*result4;
	db *x_input,*x_out;
	x_input->x =1;
	x_input->y =2;
	x_out->x=1;
	int number;
	if(factor->x == 0)
	{	return NULL;  }
	if(factor->x>0 && factor->y>0 && k==1)
	{
		 ln(factor,result);
		ln(x_input,result1);
		division(result,result1,result2); /* result2 has the log10(alpha)/log10(0.5) */
		number = (int)(result2->x/result2->y); /* gives the floor value */
		x_out->y=PWR_TWO(number);
		return x_out; /* Returns 1/2^Number */
	}
	
	else
	{
		ln(factor,result);
		ln(x_input,result1);
		division(result,result1,result2); /* result2 has the log10(alpha)/log10(0.5) */
		number =((int)(result2->x/result2->y))+1; /* Gives the ciel value of the result2 */
		x_out->y=PWR_TWO(number); /* x_out now has 1/2^Number */
		minus(factor,x_out,result3); /*  performs alpha-result */
		add(aaf_calc(result3,k-1),x_out,result4); /* Performs AAF((alpha-result),k-1)+result */
		return result4;
	}
}

/********************** PICK CPU ************************

	* ops: pointer to the struct scheduler 
	* v: pointer to the struct VCPU
	********* RETURN ********
	* cpu_no: Retruns picked Cpu-Number */

static int AAF_pick_cpu(struct scheduler *ops, struct vcpu *v)
{
	cpumask_t cpus;
	cpumask_t *online;
	int cpu_no;
	/* Stores in cpus cpumask_t the mask of online CPUS on which the domains can run (soft affinity) */
	online = cpupool_scheduler_cpumask(v->domain->cpupool);
	/* cpumask_and(destination,src1,src2); 
	 * Now do an intersection of available and onlie CPUs and store the result in &cpus */
	cpumask_and(&cpus,online,v->cpu_hard_affinity); 
	/* Now try to find if the CPU on which the current VCPU residing is available or not 
	 * Else, proceed with finding other available PCPUs/CPUs.
	
	 * int cpumask_test_cpu(cpu,mask): true iff cpu is set in mask , else: cpumask_cycle */
	cpu_no = cpumask_test_cpu(v->processor,&cpus)? v->processor:
						     cpumask_cycle(v->processor,&cpus);
	/* Assert if mask is empty or set and also check if cpu_no is set in the mask or not */
	ASSERT(!cpumask_empty(&cpus) && cpumask_test_cpu(cpu_no,&cpus));
	/* If assert success: then return cpu_no */
	return cpu_no;
}
/******************* PICK CPU ENDS **********************************/

/********************************** INit PCPU ***********************

	* Setting of CPU mask done in init_pdata
	* ops: a const struct pointer to scheduler
	* cpu: bit cpu in the cpumask 
	***** return ***
	* none */
static void AAF_init_pdata(const struct scheduler *ops,int cpu)
{
	unsigned long flags;
	/* Linking up struct scheduler's sched_data to AAF_private_info */
	struct AAF_private_info *prv = AAF_PRIV_INFO(ops);
	/* protect the space between locks */
	spin_lock_irqsave(&prv->lock,flags);
	/* Now, time to turn on the cpu  bit inside the mask */
	cpumask_set_cpu(cpu,&prv->cpus);
	spin_unlock_irqrestore(&prv->lock,flags);
	printk(KERN_ERR "Error in PData Init");
}

/********************* ALLOC_PDATA *******************
	* ops: pointer to the struct shceduler
	* cpu: cpu_no
	****** return *****
	* returns pcpu */
static void * AAF_alloc_pdata(const struct scheduler *ops,int cpu)
{
	struct AAF_pcpu *pc;
	pc = xzalloc(struct AAF_pcpu);
	/* Initialize the linked list inside the AAF_pcpu (if any) */
	INIT_LIST_HEAD(&pc->time_list);
	return pc;
}

/******************** Free_pdata ***********************
	* ops: pointer to the struct scheduler
	* pcpu: void pointer
	* cpu : cpu number 
	
	***** return *****
	* None */
static void *AAF_free_pdata(const struct scheduler *ops, void *pcpu, int cpu)
{
	/* pcpu either points to a valid pcpu or a NULL pointer, so xfree has to be called
	 * after deiniting the p_data with deinit_pdata */
	xfree(pcpu);
}

/************* DEINIT_PDATA ***********************
	* removal of CpU mask done in deinit _cpu	

	* ops: pointer to the struct scheduler
	* pcpu: void pointer
	* cpu: int cpu_number
	******* return ******
	* returns NONE  **/
static void AAF_deinit_pdata(const struct scheduler *ops, void *pcpu, int cpu)
{
	struct AAF_pcpu *pc = pcpu; /* docking the void pointer to struct AAF_pcpu */
	struct AAF_private_info *prv = AAF_PRIV_INFO(ops);
	/* Check if we are deinitializing the intended cpu and its bit in the cpumask */
	ASSERT(pc && cpumask_test_cpu(cpu,&prv->cpus));
	/* If that, clear the cpumask on that cpu and it;s bit in the cpumask */
	cpumask_clear_cpu(cpu, &prv->cpus);
}

/********************** DOMAIN ALLOCATIONS ******************
	* ops: pointer to the struct scheduler
	* dom: pointer to the struct domain

	************ return ********
	* none */
#define ALLOC_DOMDATA
#ifdef ALLOC_DOMDATA
static void *AAF_alloc_domdata(struct scheduler *ops, struct domain *dom)
{
	
	struct AAF_dom *doms;
	struct AAF_partition *par;
	
	db *al;
	doms = xzalloc(struct AAF_dom);

	/* Partition(s) residing inside the domains have to be initialized here 
	 * as there is no explicit function calls for the partitions alloc,free,init and deinit. */
	
       par = xzalloc(struct AAF_partition);
	
	par->alpha->x = par->time_slices;
        par->alpha->y = par->period;

	al = xzalloc (db);
	al->x = 1;
	par->alpha=al;
	
	/* avail is being fed as user input and hence must come from domain *dom */
	/* Linked List Initializations of both domains and partitions within */
	INIT_LIST_HEAD(&doms->dom_list);
	INIT_LIST_HEAD(&par->vcpu_list); 
}

/*************************** DOMAIN DEALLOCATION ********************
	*ops: pointer to the struct scheduler
	*dom: pointer to the void 
	******* Return ******
	* returns none ***/
static void *AAF_free_domdata(const struct scheduler *ops,void *dom)
{
	struct AAF_partition *aaf_par;
	struct AAF_dom *aaf_dom = dom;
	aaf_par = aaf_dom->inter_par;
	/*aaf_par = aaf_dom->inter_par;*/ 
	/* Doing an xfree(dom) should also erase the partitions residing inside, however it
	 * is still a good practice to do an xfree of the partitions inside */	
	xfree(aaf_par);
	xfree(aaf_dom);
}
#else
#endif

/************************ AAF_PRIVATE INIT AND DENIT  ************************

    * NOTE ::: DO NOT initialize the instance of the strcut scheduler with a const when it is passed as an 
	   ::: argument to AAF_init and AAF_deinit as we dock and undock it with AAF_private_info 

	*ops: pointer to the struct scheduler
	***** RETURN *****
	* 0 : success
	* !0: Error in initialization  */
static int AAF_init(struct scheduler *ops)
{
	struct AAF_private_info *prv;
	prv = xzalloc(struct AAF_private_info);
	if(prv == NULL)	
	{
		return -ENOMEM; /* Return nothing if  AAF_private_info is void  */
	}

	/* Now dock the AAF_private_info to the scheduler's sched_data provided AAF_private is not empty */
	ops->sched_data = prv;
	/* Global lock init */
	spin_lock_init(&prv->lock);
	INIT_LIST_HEAD(&prv->ndom); 
	INIT_LIST_HEAD(&prv->vcpu_list);
	return 0;
}

/********************* DEINITIALIZATION OF THE INSTANCE OF A SCHEDULER ***********
	*ops: Pointer to the struct scheduler
	****** RETURN ********
	* none */
static void AAF_deinit( struct scheduler *ops)
{
	xfree(AAF_PRIV_INFO(ops));
	ops->sched_data = NULL; /* To ensure that the current scheduler is 
				 * properly undocked from the global scheduler */
}




/******************************************************************************************/
/* Work is in progress, do not release #ifdefs until the functions are fully developed */
#ifndef __AAF_SINGLE__

 const struct scheduler sched_aaf =
{
        .name = "AAF Scheduler",
         .opt_name = "aaf",
        .sched_id = XEN_SCHEDULER_AAF, /* sched_id of AAF has to be registered later on */
        
        /* Scheduler Init Functions */
        .init = AAF_init,
        .deinit = AAF_deinit,

        /* PCPU Functions */
	.pick_cpu = AAF_pick_cpu,
        .alloc_pdata = AAF_alloc_pdata,
        .init_pdata = AAF_init_pdata,
        .deinit_pdata = AAF_deinit_pdata,
        .free_pdata = AAF_free_pdata,

        /* Domain Allocation and Freeing */
        .alloc_domdata = AAF_alloc_domdata,
        .free_domdata = AAF_free_domdata,
        
        /* VCPU Allocation */
        .alloc_vdata = AAF_alloc_vdata,
        .free_vdata  = AAF_free_vdata,
        .insert_vcpu = AAF_insert_vcpu,
        .remove_vcpu = AAF_remove_vcpu,

        /***************Need modification list****************/
        /*
        *.init = AAF_init,
        *.deinit = AAF_deinit,


        *.alloc_pdata = AAF_alloc_pdata,
        *.init_pdata = AAF_init_pdata,
        *.deinit_pdata = AAF_deinit_pdata,
        *.free_pdata = AAF_free_pdata,


        *.alloc_domdata = AAF_alloc_domdata,
        *.free_domdata = AAF_free_domdata,
        

        *.alloc_vdata = AAF_alloc_vdata,
        *.free_vdata  = AAF_free_vdata,
        *.insert_vcpu = AAF_insert_vcpu,
        *.remove_vcpu = AAF_remove_vcpu,
        */
        /*****************************************************/


};
#else
#endif


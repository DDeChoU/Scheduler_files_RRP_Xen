
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

/* Given a CPU number, retreive its RUNQ */
#define RUNQ(_cpu) (&AAF_PCPU(_cpu)->runq)


	

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
	* hp: 		Have a unique and global hyperperiod for the entire system 
	*/

    spinlock_t lock;
    cpumask_t cpus;
   s_time_t hp;
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
	struct list_head *time_now; /*pointer to the time_list, so no INIT_LIST_HEAD for this */
	struct list_head runq /* Maintaining a runQ inside each PCPU */
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
    struct list_head vcpu_list; /* this list binds it to the partition,head of which is in AAF_partition */
    struct list_head runq_elem; /* VCPU as an element whose Queue is in the PCPU as runQ */ 
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
   /* spinlock_t dom_lock;*/
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
	* partition_list:   	list head of the partitions 
	*/
 
	unsigned int par_id;
	struct list_head vcpu_list;
	struct list_head *vcpu_now;
	struct list_head partition_list;
	/* Alpha re-introduced as an int(x)/int(y) parameter */
	s_time_t time_slices; /* These parameters come from user end */
	s_time_t period;
	db *alpha;
	int k;
	spinlock_t par_lock;
	db  (*aaf_calc)(db *alpha,int k); /*Moved to AAF_partition*/
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
	struct AAF_dom *dom_ptr;
};


/******************************************************************************************/

/****************************Assistance Functions**************************************/


/***************** FUNCTION TO RETREIVE A PARTITION GIVEN PARTITION'S DOMAIN ID ********
	* Since the current setup only supports one Partition per domain,
	* we can as well make use of the domain_handle to retreive the partition residing in it 
	* Provide the domain id and the function will return the concerned partition as every domain has only 1 partition */

static inline struct AAF_partition *get_AAF_par(struct AAF_dom *dom_ptr)
{
	
	/* Iterate through the list of domains to see if it matches dom_id */
	
		return dom_ptr->inter_par;
	
}

static inline void swap(int * a, int * b)
{
    int temp = *a;
    *a = *b;
    *b = temp;
}

/*Adjust each element to a reasonable place in the maximum heap*/
static inline void down_adjust(int *arr, int i, int n)
{
    int son = i*2+1, parent = i;
    while(son<n)
    {
        if(son+1<n && arr[son+1] > arr[son])
            son ++;
        if(arr[parent] > arr[son])
            return;
        swap(&arr[parent], &arr[son]);
        parent = son;
        son = parent * 2 + 1;
    }
}

/*Heap sort and insert elements into the corresponding pcpu */
/* Returning sorted timeslices */
static inline void heap_sort_insert(int* arr, int n, int pcpu, struct AAF_dom *dom_ptr)
{
    int counter = 0;
    struct AAF_pcpu* apcpu = AAF_PCPU(pcpu); 
    struct time_slice* t;
    struct list_head *head = &apcpu->time_list, *end = (head)->prev, *temp = end;
    /*initialize the heap*/
    for(counter = n/2 -1; counter >= 0; counter --)
    {
        down_adjust(arr, counter, n);
    }
    for(counter = n-1; counter>0; counter--)
    {
        /*arr[0] is now the largest time slice, insert it from the back
         * Utilize functions list_last_entry and list_prev_entry to accomplish the insertion.
         * Mind that the index of the time slice needs to be mentioned in struct timeslice.
        */

        while(temp!=head && list_entry(temp, struct time_slice, time_list)->index > arr[counter])
        {
            temp = temp->prev;
        }
        /*Find the correct place to insert and then initialize the timeslice and insert it into the linked list*/
        t = xzalloc(struct time_slice);
        t->dom_ptr = dom_ptr;
        INIT_LIST_HEAD(&t->time_list);
        t->index = arr[counter];
        list_add(&t->time_list, temp);
        
        swap(&arr[0], &arr[counter]);
        down_adjust(arr, 0, counter - 1);


    }
}


/************ SWITCH SCHEDULER FROM CURRENT TO AAF_XEN OF A CPU ***********************
	* new_ops: pointer to the struct scheduler
	* cpu: CPU number, the cpu that is changing the scheduler
	* pdata: Scheduler specific PCPU data
	* vdata: Scheduler specific VCPU data of the idle VCPU
	return : NULL **/
static void AAF_switch(struct scheduler *new_ops, unsigned int cpu, void *pdata, void *vdata)
{
	struct schedule_data *sd = &per_cpu(schedule_data, cpu); /* per_cpu returns only one isntance of the struct scheduler */
	struct AAF_vcpu *vc = vdata; /* Docking arg with this scheduler's vcpu) */
	struct aaf_pcpu *pc = pdata; /* Docking arg (pdata) with this Scheduler's pcpu struct */
	/* Assert if AAF's pcpus exist ,vcpus and these vcpus are idle */
	ASSERT(pc && vc && is_idle_vcpu(vc->vcpu));
	
	/* No locks are being put on the scheduler now, will have to, in future ~! */
	idle_vcpu[cpu]->sched_priv = vdata;
	/* As we have Struct PCPu, we need to init_pdata while switching scheduler */
	/* init_pdata retruns the ID of the runQ on the CPU it is running */
	/* IN PROGRESS -- NEED TO INCLUDE THE RUNQ SWITCHES __ */
}


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
	
	struct AAF_private_info *pv;
	s_time_t temp_res_num,temp_res_denom;
	if(pv->hp==0)
	{
		pv->hp = par->alpha->y;
	}
	else
	{
		/* Numerator of the LCM stored in temp_res */
		temp_res_num = (par->alpha->y)*(pv->hp);
		temp_res_denom = (gcd(par->alpha->y,pv->hp));
		pv->hp = (s_time_t)(temp_res_num/temp_res_denom);

		/*pc->hp = (par->alpha.period)*(pc->hp)/GCD((par->period),pc->hp);*/
	}
	return (pv->hp);

	/* provides us with the index of the time slice corresponding to a partition */
	/*ts->index*/ 
}

/******************** AAF CALC FUNCTION ********************
	* struct alpha
	* int k: Supply regularity 
      ****** Return value ******
	* returns a struct of type double */
db aaf_calc(db *factor,int k)
{
	db result,result1,result2,result3,result4;
	db x_input,x_out,temp;
	x_input.x =1;
	x_input.y =2;
	x_out.x=1;
	int number;
	if(factor->x == 0)
	{	return;  }
	if(factor->x>0 && factor->y>0 && k==1)
	{
		 ln(factor,&result);
		ln(&x_input,&result1);
		division(&result,&result1,&result2); /* result2 has the log10(alpha)/log10(0.5) */
		number = (int)(result2.x/result2.y); /* gives the floor value */
		x_out.y=PWR_TWO(number);
		return x_out; /* Returns 1/2^Number */
	}
	
	else
	{
		ln(factor,&result);
		ln(&x_input,&result1);
		division(&result,&result1,&result2); /* result2 has the log10(alpha)/log10(0.5) */
		number =((int)(result2.x/result2.y))+1; /* Gives the ciel value of the result2 */
		x_out.y=PWR_TWO(number); /* x_out now has 1/2^Number */
		minus(factor,&x_out,&result3); /*  performs alpha-result */
		temp = aaf_calc(&result3,k-1);
		add(&temp,&x_out,&result4); /* Performs AAF((alpha-result),k-1)+result */
		return result4;
	}
}



/************************ AAF_SINGLE_ALGORITHM *************************
	* Input: Partition Set 
	* Returns sorted list of time slices 
**/

static inline void AAF_single(const struct scheduler *ops)
{
	struct AAF_dom *dom;
	struct AAF_private_info *prv;
	double maxaaf;
	int w=1;
	s_time_t firstAvailableTimeSlice=0;
	db temp,temp1;
	/* Function hyperperiod takes in a pointer to the partition, so we need to iterate it through
	* the list of partitions and calculate cumulative hyperperiod of the system */
	
	/* iterate through the list of domains and collect the cumulative hyperperiod of all the 
	 * partitions residing in every domain */
	int level=0,counter,i=0,hyperp;
	db p;
	p.x=1;
	temp.x=1;
	temp.y=10000;
		/* iterates through the list of domains */
		list_for_each_entry(dom,&AAF_PRIV_INFO(ops)->ndom,dom_list)
			{	
			prv->hp = hyperperiod(dom->inter_par); /* cumulative hp of all the domains */
			counter++; /* Gives the final count value of num of
							    * domains */
			}
	/* Memory Allocation for uni dimensional arrays using sizeof */
	s_time_t t_p_counter[sizeof(s_time_t)*counter];
	s_time_t distance[sizeof(s_time_t)*counter];
	/* 1st array assignment of number of domains */
	/* 2nd array assignment of hyperperiod */
	hyperp = (int) prv->hp;
	/* *t_p=(s_time_t **)malloc(counter*sizeof(s_time_t*)); */
	/*(s_time_t**) (*t_p)[sizeof(s_time_t*)*counter];  2D array mem alloc */
	s_time_t t_p[sizeof(s_time_t)*counter][sizeof(s_time_t)*hyperp]; /* Attempt 2 */
	

	while(counter>0)
	{

				p.y = TWO_PWR(level);
				w = (int) (p.x/p.y); /* Forced conversion into int */
				int tsize = w*(int)(prv->hp);
		/* prv->hp after iterating through all the domains in the environment has a unique and
	 	 * a final hyperperiod for the entire system now */

                   /* iterate through the list of domains */
		list_for_each_entry(dom,&AAF_PRIV_INFO(ops)->ndom,dom_list)
		{
			int l;
			int num = (int)(dom->inter_par->aaf_calc(dom->inter_par->alpha,dom->inter_par->k).x);
			int denom =  (int)(dom->inter_par->aaf_calc(dom->inter_par->alpha,dom->inter_par->k).y);
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
			temp1= dom->inter_par->aaf_calc(dom->inter_par->alpha,dom->inter_par->k);
			if(less_than(&temp1,&temp)==0) 
				counter--;
			    	
			}
		}
		level++;
	}
	i=0;
	list_for_each_entry(dom, &AAF_PRIV_INFO(ops)->ndom, dom_list)
	{
		heap_sort_insert(t_p[i],t_p_counter[i], smp_processor_id(),dom);
		i++;
	
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
	
	/*

	online = cpupool_scheduler_cpumask(v->domain->cpupool);
	
	*/
	/* cpumask_and(destination,src1,src2); 
	 * Now do an intersection of available and onlie CPUs and store the result in &cpus */
	/*

	cpumask_and(&cpus,online,v->cpu_hard_affinity); 

	*/

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
	list_add(&AAF_PRIV_INFO(ops)->ndom, &doms->dom_list);
	AAF_single(ops);

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


/******************* VCPU INITS AND ALLOCATIONS *********************
	*ops: pointer to the struct scheduler
	* vc: pointer to the struct vcpu
	* void *: A void pointer useful for typecast in future
	**** RETURN VALUE *********
	* None: returns nothing ***/
static void *AAF_alloc_vdata(const struct scheduler *ops, struct vcpu *vc, void *dd)
{
	struct AAF_private_info *sched_priv = AAF_PRIV_INFO(ops);
	 struct AAF_vcpu *vcp;
	unsigned long flags;
	unsigned int entry;
	/* Allocate memory for the AAF_specific scheduler vcpu */
	vcp = xzalloc(struct AAF_vcpu);
	if( vcp == NULL)
		return NULL;
	INIT_LIST_HEAD(&vcp->vcpu_list);
	/** docking **/
	vcp->vcpu = vc;
	vcp->dom = dd;
	/* Counter set-up (if necessary ) */
	SCHED_STAT_CRANK(vcpu_alloc);
	return vcp;
}

static void AAF_free_vdata(const struct scheduler *ops, void *priv)
{
	struct AAF_vcpu *vc = priv;
	xfree(vc);
}

/******************** DO SCHEDULE *****************
	* ops: pointer to the struct scheduler
	* now: an s_time_t parameter
	* task_scheuled : bool_t parameter
	**** return ***
	* returns an instance of task slice **/
static struct task_slice AAF_schedule(const struct scheduler *ops, s_time_t now, bool_t task_scheduled)
{
	struct task_slice ret;
	struct list_head *pos,*pos1;
	struct time_slice *ts;
	struct AAF_partition *pars;
	struct AAF_vcpu *vc,*vcpus=NULL;
	ret.time = MILLISECS(10);
	/* FOr starters, we assume there is a partition-time slice table and the index of partition
	* in that table is merely the Domain ID as for now, we assume each domain has a single partition */
	unsigned int cpu = smp_processor_id();
	struct AAF_pcpu *pc = AAF_PCPU(cpu); /* we have access to the struct pcpu with the help of CPU number now */
	pc->time_now = pc->time_now->next;
	/* Assuming we have a partition/ domain number in the partition-timeslice table inside of each PCPU */
	if(pc->time_now == &pc->time_list)
	{	
		pc->time_now=pc->time_now->next;
	}
	pos=pc->time_now;
	ts = list_entry(pos, struct time_slice, time_list);
	pars = get_AAF_par(ts->dom_ptr); /* gets partition */
	/* A list head */
	if(list_empty(&pars->vcpu_list))
		return ret;/*The empty NULL may cause run-time error*/
	/* We get the VCPU whch is currently the pointer is pointing to */
	vcpus = list_entry(pars->vcpu_now, struct AAF_vcpu, vcpu_list);
	pos1 = pars->vcpu_now;
	/* If this is the only vcpu in the list, put it in the ret.task and break */
	if(list_is_singular(pars->vcpu_now)==0)
	{
		ret.task = vcpus;
		return ret;
	}
	else
	{
		while(!vcpu_runnable(vcpus))
		{
			if(pars->vcpu_now==pos1)
				break;
		
			pars->vcpu_now = pars->vcpu_now->next;
			if(pars->vcpu_now = &pars->vcpu_list)
				pars->vcpu_now = pars->vcpu_now->next;
			vcpus = list_entry(pars->vcpu_now, struct AAF_vcpu, vcpu_list);
		}
		ret.task = vcpus;
	}
	return ret;
}
/******************************************************************************************/
/* Work is in progress, do not release #ifdefs until the functions are fully developed */
#ifdef __AAF_SINGLE__

 const struct scheduler sched_aaf =
{
        .name = "AAF Scheduler",
         .opt_name = "aaf",
        .sched_id = XEN_SCHEDULER_CREDIT2, /* sched_id of AAF has to be registered later on */
        
        /* Scheduler Init Functions */
        .init = AAF_init,
        .deinit = AAF_deinit,
	.switch_sched = AAF_switch,
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
        .insert_vcpu = NULL, /* NULL since there is no global queue being maintained */
        .remove_vcpu = NULL,

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


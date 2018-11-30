/************** AAF ******************
* Developers : Pavan Kumar Paluri & Guangli Dai
* AAF: Algorithm that creates fixed partitions and assisgns them
* to PCPUs based on the concept of levels and AAF
* Latest Modification : 11/30/2018
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
#define TIMESLICERRAYSIZE (65536)
#define AAF_COMP_MIN (MICROSECS(5))
#define CALCULATE_W(level) \
			(1.0/POWER(2,level))
#define APPROX_VAL(val) (floor(val))


/* can freely access the global data pointer coming from struct scheduler */
/* Hence typecasting it to type struct AAF_private_info */
#define AAF_PRIV_INFO(_d) (\
				(struct AAF_private_info *)(_d)->sched_data)
#define AAF_PCPU(_pc) \
		((struct aaf_pcpu *)per_cpu(schedule_data,_pc).sched_priv)
#define AAF_DOM(_dom) ( \
			(struct AAF_dom *)(_dom)->sched_priv)

/****************************Definition of Structures**************************************/

struct AAF_private_info
{
	/*
	* Variables:
	* sched_lock:	Spinlock for the scheduler.
	* cpus:			cpumask_t for all available physical CPUs.
	* ndom:			A linked list that track how many domains are under the control of this scheduler.
	* 
	* par_count:	Partition count, not used yet.
	*/

    spinlock_t sched_lock;
    cpumask_t cpus;
    struct list_head ndom;

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
	struct list_head *time_now;
}


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
}

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
}

struct AAF_partition
{
	/*
	* Variables:
	* par_id:		The partition id.
	* vcpu_list:	The head of the linked list holding vcpus.
	* ava:			Availability factor of the partition.
	* reg:			Regularity of the partition.
	* par_lock:		Spinlock for the partition.
	*/

	unsigned int par_id;
	struct list_head vcpu_list;
	double ava;
	int reg;
	spinlock_t par_lock;
}

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
}



/******************************************************************************************/

/****************************Assistance Functions**************************************/


/******************************************************************************************/


 const struct scheduler sched_aaf =
{
        .name = "AAF Scheduler",
        .opt_name = "aaf",
        .sched_id = XEN_SCHEDULER_AAF, /* sched_id of AAF has to be registered later on */
        
        /* Scheduler Init Functions */
        .init = AAF_init,
        .deinit = AAF_deinit,

        /* PCPU Functions */
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
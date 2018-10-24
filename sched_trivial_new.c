/*************************************************************
 * A TRIVIAL SCHEDULER - ROUND ROBIN FASHION
 *  File: xen/common/sched_trivial.c
 *  Author: Pavan Kumar Paluri & Guangli Dai
 *  Description: Implements a trivial scheduler that schedules VCPUS in a round-robin fashion
 */

#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/sched-if.h>
#include <xen/trace.h>
#include <xen/softirq.h>
#include <xen/keyhandler.h>
#include <xen/init.h>
#include <xen/delay.h>
#include <xen/event.h>
#include <xen/time.h>
#include <xen/perfc.h>
#include <xen/softirq.h>
#include <asm/div64.h>
#include <xen/errno.h>
#include <xen/cpu.h>
#include <xen/keyhandler.h>


/* macros that simplifies the connection */
#define TRIVIAL_VCPU(_vcpu)   ((struct trivial_vcpu *)(_vcpu)->sched_priv)

/*Maintaining a RUN_Q for VPCUs in the VCPU*/

/*
*System-wide private data for algorithm trivial
*/
struct trivial_private
{
    spinlock_t lock;        /* scheduler lock; nests inside cpupool_lock */
    struct list_head ndom;  /* Domains of this scheduler */
    /*
        Compared to sched_null, we do not maintain a waitq for vcpu and do not maintain a cpus_free.
    
    */


};


/*
 *Trivial PCPU, not sure what this is used for.
 */

struct trivial_pcpu
{
    struct list_head waitq_elem;
    struct vcpu *vcpu;
}

/*
 *Trivial VCPU
 */
struct trivial_vcpu {
struct list_head runq;/*The linked list here maintains a run queue */
struct vcpu *vcpu;
};
/* 
*************************************************************************
Assistance function 
*************************************************************************
*/
static inline struct trivial_private* get_trivial_priv(const struct scheduler *ops)
{
    return ops->sched_data;
}

static inline void insert_runq(struct trivial_vcpu *tvc)
{
    /* null is using the cpu to link back to the sched_vcpu, we can follow the credit style, 
    but it seems we need to pick a cpu before we insert anyway. use list_add_tail to insert */
}

static void init_pdata(struct trivial_private* prv, unsigned int cpu )
{
    /* Mark the PCPU as free, initialize the pcpu as no vcpu associated  */
    cpumask_set_cpu(cpu, &prv->cpus_free);
    per_cpu(npc, cpu).vcpu = NULL;
}


/* 
*************************************************************************
Core functions needed by interfaces in scheduler 
*************************************************************************
*/

static int trivial_init(struct scheduler* ops)
{
/* this function is used to initialize the sched_data, it should be a class containing lock and other critical info. */
    struct trivial_private *prv;

    prv = xzalloc(struct trivial_private);
    if(prv == NULL)
        return -ENOMEN;

    spin_lock_init(&prv->lock);
    INIT_LIST_HEAD(&prv->ndom);

    ops->sched_data = prv;

    return 0;
}

static void trivial_deinit(struct scheduler *ops)
{
    /*
        free the instance stored in ops->sched_data 
    */
    xfree(ops->sched_data);
    ops->sched_data = NULL;
}

static void trivial_init_pdata(const struct scheduler *ops, void *pdata, int cpu)
{
    struct trivial_private *prv = get_trivial_priv(ops);
    struct schedule_data * sd = &per_cpu(schedule_data, cpu);
/*
    Omit the assert below for the time being.
    ASSERT(!pdata);

    
     * The scheduler lock points already to the default per-cpu spinlock,
     * so there is no remapping to be done.
     
    ASSERT(sd->schedule_lock == &sd->_lock && !spin_is_locked(&sd->_lock));


*/
    init_pdata(prv, cpu);
}


static void trivial_switch_sched(struct scheduler *new_ops, unsigned int cpu,
                                 void *pdata, void *vdata)
{
    struct schedule_data *sd = &per_cpu(schedule_data, cpu);
    struct trivial_private *prv = get_trivial_priv(new_ops);
    struct trivial_vcpu *tvc = vdata;

    ASSERT(tvc && is_idle_vcpu(tvc->vcpu));

    idle_vcpu[cpu]->sched_priv = vdata; 
    /* Not sure what this is for, idle_vcpu is an array defined in sched.h.
       The rest is similar in credit and null, it should work for trivial as well.
     */

    /*
     * We are holding the runqueue lock already (it's been taken in
     * schedule_cpu_switch()). It actually may or may not be the 'right'
     * one for this cpu, but that is ok for preventing races.
     *
     *init_pdata below clear all allocation before.
     */
    ASSERT(!local_irq_is_enabled());

    init_pdata(prv, cpu);

    per_cpu(scheduler, cpu) = new_ops;
    per_cpu(schedule_data, cpu).sched_priv = pdata;

    /*
     * (Re?)route the lock to the per pCPU lock as /last/ thing. In fact,
     * if it is free (and it can be) we want that anyone that manages
     * taking it, finds all the initializations we've done above in place.
     */
    smp_mb();
    sd->schedule_lock = &sd->_lock;

}

static void trivial_deinit_pdata(const struct scheduler *ops, void *pcpu, int cpu)
{
    struct trivial_private *prv = get_trivial_priv(ops);

    ASSERT(!pcpu);

    cpumask_clear_cpu(cpu, &prv->cpus_free);
    per_cpu(npc, cpu).vcpu = NULL;
}



static void *trivial_alloc_vdata(const struct scheduler *ops,
                                 struct vcpu *v, void *dd)
{
    struct trivial_vcpu *tvc;
    tvc = xzalloc(struct trivial_vcpu);
    if (tvc == NULL)
            return NULL;
    INIT_LIST_HEAD(tvc->runq);
    tvc->runq = v;

    SCHED_STAT_CRANK(vcpu_alloc);

    return tvc;
}

static void trivial_free_data(const struct scheduler * ops, void *priv)
{
    struct trivial_vcpu *tvc = priv;

    xfree(tvc);
}

static void insert_trivial_vcpu(const struct scheduler *ops,struct vcpu *v)
{
    /* BUG(); not touched before the page fault*/
    /*
        1. do cpu_pick() : choose a cpu that the vcpu will be working on. Use lock when picking.
        2. Add the vcpu into the runq, use lock as well.
    
    */

    if(vcpu_list_head==NULL)
    {
        vcpu_list_head=v;
        vcpu_list_tail=v;
    }
    else {
        vcpu_list_tail->sched_priv = vcpu_list_tail;
        vcpu_list_tail =v;
    }
    v->sched_priv=NULL;
       /* return 0;*/
    /* BUG();  not reached, page fault occurs before this. */
}



/* make an interface (definition structure)*/

const struct scheduler sched_trivial_def =
        {
                .name           = "Trivial Round Robin Scheduler",
                .opt_name       = "trivial",
                .sched_id       = XEN_SCHEDULER_CREDIT,
                .sched_data     = NULL,

                .init           = trivial_init,
                .deinit         = trivial_deinit,
                .init_pdata     = trivial_init_pdata;
                .switch_sched   = trivial_switch_sched,
                .deinit_pdata   = trivial_deinit_pdata;

                .alloc_vdata = trivial_alloc_vdata,
                .free_vdata = trivial_free_data,
                /*
                    functions above are all set.

                */
                .insert_vcpu = insert_trivial_vcpu,
                .remove_vcpu = trivial_destroy_vcpu,
                .do_schedule = trivial_sched,

      };
REGISTER_SCHEDULER(sched_trivial_def);

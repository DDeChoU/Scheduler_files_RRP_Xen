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
    struct list_head runq;
    spinlock_t runq_lock;
    struct list_head *list_now;
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
};

/*
 *Trivial VCPU
 */
struct trivial_vcpu 
{
    struct list_head runq_elem;/*The linked list here maintains a run queue */
    struct vcpu *vcpu;
};

/*
*Trivial Domain
*/
struct trivial_dom
{
    struct list_head tdom_elem;
    struct domain *dom;
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

static inline struct trivial_vcpu *get_trivial_vcpu(const struct vcpu *v)
{
    return v->sched_priv;
}


static inline void init_pdata(struct trivial_private* prv, unsigned int cpu )
{
    /* Mark the PCPU as free, initialize the pcpu as no vcpu associated 
    cpumask_set_cpu(cpu, &prv->cpus_free);
    per_cpu(npc, cpu).vcpu = NULL;
    */
}


/*takes in the head of the linked list and return the end of the linked list. If the list is empty, return NULL.
 *Since it is a circular linked list, just return the previous of the head.
*/




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
        return -ENOMEM;

    spin_lock_init(&prv->lock);
    spin_lock_init(&prv->runq_lock);
    INIT_LIST_HEAD(&prv->ndom);
    INIT_LIST_HEAD(&prv->runq);
    ops->sched_data = prv;
    prv->list_now = &prv->runq;
    printk(KERN_ERR "INITIALIZATION");
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

    printk(KERN_ERR "INIT_PData");
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

    printk(KERN_ERR "SWITCH_SCHED");
}

static void trivial_deinit_pdata(const struct scheduler *ops, void *pcpu, int cpu)
{
   /* struct trivial_private *prv = get_trivial_priv(ops);

    ASSERT(!pcpu);

   cpumask_clear_cpu(cpu, &prv->cpus_free);
    per_cpu(npc, cpu).vcpu = NULL;*/
}



static void *trivial_alloc_vdata(const struct scheduler *ops,
                                 struct vcpu *v, void *dd)
{
    struct trivial_vcpu *tvc;
    tvc = xzalloc(struct trivial_vcpu);
    if (tvc == NULL)
            return NULL;
    INIT_LIST_HEAD(&tvc->runq_elem);
    tvc->vcpu = v;

    SCHED_STAT_CRANK(vcpu_alloc);

    printk(KERN_ERR "ALLOC_VDATA");
    return tvc;
}

static void trivial_free_vdata(const struct scheduler * ops, void *priv)
{
    struct trivial_vcpu *tvc = priv;

    xfree(tvc);
}

static void *trivial_alloc_domdata(const struct scheduler *ops, struct domain *d)
{
    /*
        One domain should have only one vcpu for RRP. Where should we specify the link between
        vcpu and domain? Is the link necessary?
        This is not given in the definition of trivial_dom for now. Trivial seems to be fine
        without the link because its schedule is not based on the domain.
        
    */

    struct trivial_private *prv = get_trivial_priv(ops);
    struct trivial_dom *tdom;
    unsigned long flags;

    tdom = xzalloc(struct trivial_dom);
    if(tdom == NULL)
        return ERR_PTR(-ENOMEM);

    tdom->dom = d;

    spin_lock_irqsave(&prv->lock, flags);
    list_add_tail(&tdom->tdom_elem, &get_trivial_priv(ops)->ndom);
    spin_unlock_irqrestore(&prv->lock, flags);

    return tdom;

}

static void *trivial_free_domdata(struct scheduler *ops, void *data)
{
    struct trivial_dom *tdom = data;
    struct trivial_private *prv = get_trivial_priv(ops);

    if(tdom)
    {
        unsigned long flags;

        spin_lock_irqsave(&prv->lock, flags);
        list_del_init(&tdom->tdom_elem);
        spin_unlock_irqrestore(&prv->lock, flags);

        xfree(tdom);
    }
}


static void trivial_insert_vcpu(const struct scheduler *ops,struct vcpu *v)
{
    /* BUG(); not touched before the page fault*/
    /*
         Add the vcpu into the runq, use lock as well.
         Should we lock or not?
    */

    struct trivial_private *prv = get_trivial_priv(ops);
    struct trivial_vcpu *tvc = get_trivial_vcpu(v);
    spin_lock(&prv->runq_lock);
    list_add_tail(&tvc->runq_elem, &prv->runq);
    spin_unlock(&prv->runq_lock);
       /* return 0;*/
    /* BUG();  not reached, page fault occurs before this. */
}

static void trivial_remove_vcpu(struct trivial_private *prv, struct vcpu *v)
{
    unsigned int cpu = v->processor;
    struct trivial_vcpu *tvc = get_trivial_vcpu(v);
    ASSERT(list_empty(&tvc->runq_elem));
    /*vcpu_deassign(prv, v, cpu);
     * Should implement vcpu_deassign here to get the vcpu out of the pcpu.	
    */
    list_del_init(&tvc->runq_elem);
    spin_lock(&prv->runq_lock);
    SCHED_STAT_CRANK(vcpu_remove);
}

static struct task_slice trivial_schedule(const struct scheduler *ops,
                                          s_time_t now,
                                          bool_t tasklet_work_scheduled)
{
    struct task_slice ret;
    struct list_head* pos;
    struct trivial_private *pri = get_trivial_priv(ops); 
    struct trivial_vcpu *tvc = NULL;
    ret.time = MILLISECS(10);
    /*BUG();*/    

    list_for_each(pos, pri->list_now)
    {
        if(pos == &pri->runq)
            continue;
        tvc = list_entry(pos, struct trivial_vcpu, runq_elem);
        if(vcpu_runnable(tvc->vcpu))
        {
            ret.task = tvc;
	    pri->list_now = &tvc->runq_elem;
	    break;
        }
    }
    /*BUG();*/ 
    /*
        ret.task = ((struct vcpu*)per_cpu(schedule_data)).idle
    */
    return ret;
    /*
    * How to increment queue here????
    */

}

const struct scheduler sched_trivial_def =
        {
                .name           = "Trivial Round Robin Scheduler",
                .opt_name       = "trivial",
                .sched_id       = XEN_SCHEDULER_CREDIT,
                .sched_data     = NULL,

                .init           = trivial_init,
                .deinit         = trivial_deinit,
                .init_pdata     = trivial_init_pdata,
                .switch_sched   = trivial_switch_sched,
                .deinit_pdata   = trivial_deinit_pdata,

                .alloc_vdata    = trivial_alloc_vdata,
                .free_vdata     = trivial_free_vdata,
                .alloc_domdata  = trivial_alloc_domdata,
                .free_domdata   = trivial_free_domdata,
                
		.insert_vcpu = trivial_insert_vcpu,
                .remove_vcpu = trivial_remove_vcpu,
                .do_schedule = trivial_schedule,

      };
REGISTER_SCHEDULER(sched_trivial_def);

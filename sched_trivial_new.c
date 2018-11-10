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
/*Tracing events.*/
#define TRC_TRIVIAL_PICKED_CPU    TRC_SCHED_CLASS_EVT(TRIVIAL, 1)
#define TRC_TRIVIAL_VCPU_ASSIGN   TRC_SCHED_CLASS_EVT(TRIVIAL, 2)
#define TRC_TRIVIAL_VCPU_DEASSIGN TRC_SCHED_CLASS_EVT(TRIVIAL, 3)
#define TRC_TRIVIAL_MIGRATE       TRC_SCHED_CLASS_EVT(TRIVIAL, 4)
#define TRC_TRIVIAL_SCHEDULE      TRC_SCHED_CLASS_EVT(TRIVIAL, 5)
#define TRC_TRIVIAL_TASKLET       TRC_SCHED_CLASS_EVT(TRIVIAL, 6)

/*
*System-wide private data for algorithm trivial
*/
struct trivial_private
{
    spinlock_t lock;        /* scheduler lock; nests inside cpupool_lock */
    struct list_head ndom;  /* Domains of this scheduler */

    spinlock_t runq_lock;
    cpumask_t cpus_free;
    /*
        Compared to sched_null, we do not maintain a waitq for vcpu and do not maintain a cpus_free.
    
    */


};


/*
 *Trivial PCPU, not sure what this is used for.
 */

struct trivial_pcpu
{
    /*struct vcpu *vcpu; put a vcpu list instead of one vcpu*/
    struct list_head runq;
    struct list_head *list_now;
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

static inline struct trivial_vcpu* get_trivial_vcpu(const struct vcpu *v)
{
    return v->sched_priv;
}

static inline struct trivial_pcpu* get_trivial_pcpu(const unsigned int cpu)
{
    return (struct trivial_pcpu *)per_cpu(schedule_data, cpu).sched_priv;
}

static inline void init_pdata(struct trivial_private* prv, struct trivial_pcpu *tpc, unsigned int cpu )
{
    /* Mark the PCPU as free, initialize the pcpu as no vcpu associated 

    
    nr_runnable is given in the csched_pcpu, may not need it without migration considered. 
    One option to fix the bug if the pick_cpu cannot find a valid pcpu.
    */
    cpumask_set_cpu(cpu, &prv->cpus_free);

}
static void vcpu_assign(struct trivial_private *prv, struct trivial_vcpu *tvc, unsigned int cpu)
{
    /*
        get from null scheduler, put vcpu into the linked list of the pcpu. (let insertion call this)
        modify the code so that the vcpu here is inserted into the cpu's linked list.
    */
    struct vcpu *v = tvc->vcpu;
    struct trivial_pcpu *p = get_trivial_pcpu(cpu);


    spin_lock(&prv->runq_lock);
    list_add_tail(&tvc->runq_elem, &p->runq);

    spin_unlock(&prv->runq_lock);
    v->processor = cpu;
    cpumask_clear_cpu(cpu, &prv->cpus_free);
    dprintk(XENLOG_G_INFO, "%d <-- %pv\n", cpu, v);

    if ( unlikely(tb_init_done) )
    {
        struct {
            uint16_t vcpu, dom;
            uint32_t cpu;
        } d;
        d.dom = v->domain->domain_id;
        d.vcpu = v->vcpu_id;
        d.cpu = cpu;
        __trace_var(TRC_TRIVIAL_VCPU_ASSIGN, 1, sizeof(d), &d);
    }
}

static void vcpu_deassign(struct trivial_private *prv, struct vcpu *v,
                          unsigned int cpu)
{
    struct trivial_vcpu * tvc = get_trivial_vcpu(v);
    list_del_init(&tvc->runq_elem);
    cpumask_set_cpu(cpu, &prv->cpus_free);

    dprintk(XENLOG_G_INFO, "%d <-- NULL (%pv)\n", cpu, v);

    if ( unlikely(tb_init_done) )
    {
        struct {
            uint16_t vcpu, dom;
            uint32_t cpu;
        } d;
        d.dom = v->domain->domain_id;
        d.vcpu = v->vcpu_id;
        d.cpu = cpu;
        __trace_var(TRC_TRIVIAL_VCPU_DEASSIGN, 1, sizeof(d), &d);
    }
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
    ops->sched_data = prv;
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
    unsigned long flags;

    spin_lock_irqsave(&prv->lock, flags);
    init_pdata(prv, pdata, cpu);
    spin_unlock_irqrestore(&prv->lock, flags);
/*
    Omit the assert below for the time being.
    ASSERT(!pdata);

    
     * The scheduler lock points already to the default per-cpu spinlock,
     * so there is no remapping to be done.
     
    ASSERT(sd->schedule_lock == &sd->_lock && !spin_is_locked(&sd->_lock));


*/

    printk(KERN_ERR "INIT_PData");
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

    init_pdata(prv, pdata, cpu);

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
   struct trivial_private *prv = get_trivial_priv(ops);

   ASSERT(!pcpu);

   cpumask_clear_cpu(cpu, &prv->cpus_free);
   
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

/* 
* Try to find a valid pcpu for the vcpu. It is OK even all CPUs are busy.
*/
static int trivial_cpu_pick(const struct scheduler *ops, struct vcpu *v)
{
    struct trivial_private *prv = get_trivial_priv(ops);
    unsigned int bs;
    unsigned int cpu = v->processor, new_cpu;
    cpumask_t *cpus = cpupool_domain_cpumask(v->domain);

    ASSERT(spin_is_locked(per_cpu(schedule_data, cpu).schedule_lock));

    for_each_affinity_balance_step( bs )
    {
        if ( bs == BALANCE_SOFT_AFFINITY && !has_soft_affinity(v) )
            continue;

        affinity_balance_cpumask(v, bs, cpumask_scratch_cpu(cpu));
        cpumask_and(cpumask_scratch_cpu(cpu), cpumask_scratch_cpu(cpu), cpus);

        /*
         * If our processor is free, or we are assigned to it, and it is also
         * still valid and part of our affinity, just go for it.
         * (Note that we may call vcpu_check_affinity(), but we deliberately
         * don't, so we get to keep in the scratch cpumask what we have just
         * put in it.) How to detect whether the pcpu is now occuppied by someone else 
         */
        if ( likely(cpumask_test_cpu(cpu, cpumask_scratch_cpu(cpu))) )
        {
            new_cpu = cpu;
            goto out;
        }

        /* If not, just go for a free pCPU, within our affinity, if any */
        cpumask_and(cpumask_scratch_cpu(cpu), cpumask_scratch_cpu(cpu),
                    &prv->cpus_free);
        new_cpu = cpumask_first(cpumask_scratch_cpu(cpu));

        if ( likely(new_cpu != nr_cpu_ids) )
            goto out;
    }

    /*
     * If we didn't find any free pCPU, just pick any valid pcpu, even if
     * it has another vCPU assigned. This will happen during shutdown and
     * suspend/resume, but it may also happen during "normal operation", if
     * all the pCPUs are busy.
     *
     * In fact, there must always be something sane in v->processor, or
     * vcpu_schedule_lock() and friends won't work. This is not a problem,
     * as we will actually assign the vCPU to the pCPU we return from here,
     * only if the pCPU is free.
     */
    cpumask_and(cpumask_scratch_cpu(cpu), cpus, v->cpu_hard_affinity);
    new_cpu = cpumask_any(cpumask_scratch_cpu(cpu));

 out:
    if ( unlikely(tb_init_done) )
    {
        struct {
            uint16_t vcpu, dom;
            uint32_t new_cpu;
        } d;
        d.dom = v->domain->domain_id;
        d.vcpu = v->vcpu_id;
        d.new_cpu = new_cpu;
        __trace_var(TRC_TRIVIAL_PICKED_CPU, 1, sizeof(d), &d);
    }

    return new_cpu;
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
    unsigned int cpu;
    spinlock_t *lock;

    lock = vcpu_schedule_lock_irq(v);
retry:
    cpu = v->processor = trivial_cpu_pick(ops, v);
    spin_unlock(lock);
    lock = vcpu_schedule_lock(v);
   cpumask_and(cpumask_scratch_cpu(cpu), v->cpu_hard_affinity,
                cpupool_domain_cpumask(v->domain));


    vcpu_assign(prv, tvc, cpu);


    /* put the vcpu into the linked list of each pcpu, do this after implementing the initialization,
       assign the vcpu whether the cpu is idle or not. */


    spin_unlock_irq(lock);




    SCHED_STAT_CRANK(vcpu_insert); 
    /* should be using the runq head
    spin_lock(&prv->runq_lock);
    list_add_tail(&tvc->runq_elem, &prv->runq);
    spin_unlock(&prv->runq_lock);
    */
       /* return 0;*/
    /* BUG();  not reached, page fault occurs before this. */
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

    /*Choose the corresponding vcpu from the corresponding cpu, use smp_processor_id()*/
    const int cpu = smp_processor_id();
    struct trivial_pcpu *tpc = get_trivial_pcpu(cpu);
    list_for_each(pos, tpc->list_now)
    {
        if(pos == &tpc->runq)
        {
            continue;
        }
        tvc = list_entry(pos, struct trivial_vcpu, runq_elem);
        if(vcpu_runnable(tvc->vcpu))
        {
            ret.task = tvc->vcpu;
            tpc->list_now = &tvc->runq_elem;
            break;
        }
    }


    return ret;
    /*
    * how to deal with the case where no vcpu is selected? The way the book gave is not applicable any more.
    */

}

static void * trivial_alloc_pdata(const struct scheduler *ops, int cpu)
{
    struct trivial_pcpu *tpc;
    tpc = xzalloc(struct trivial_pcpu);
    INIT_LIST_HEAD(&tpc->runq);
    tpc->list_now = &tpc->runq;

    return tpc;
}

static void trivial_free_pdata(const struct scheduler *ops, void *pcpu, int cpu)
{
    struct trivial_private *prv = get_trivial_priv(ops);
    xfree(pcpu);
}

static void trivial_vcpu_wake(const struct scheduler *ops, struct vcpu *v)
{
    ASSERT(!is_idle_vcpu(v));

    if ( unlikely(curr_on_cpu(v->processor) == v) )
    {
        SCHED_STAT_CRANK(vcpu_wake_running);
        return;
    }

    if ( unlikely(!list_empty(&get_trivial_vcpu(v)->runq_elem)) )
    {
        /* Not exactly "on runq", but close enough for reusing the counter */
        SCHED_STAT_CRANK(vcpu_wake_onrunq);
        return;
    }

    if ( likely(vcpu_runnable(v)) )
        SCHED_STAT_CRANK(vcpu_wake_runnable);
    else
        SCHED_STAT_CRANK(vcpu_wake_not_runnable);

    /* Note that we get here only for vCPUs assigned to a pCPU */
    cpu_raise_softirq(v->processor, SCHEDULE_SOFTIRQ);
}

static void trivial_vcpu_sleep(const struct scheduler *ops, struct vcpu *v)
{
    ASSERT(!is_idle_vcpu(v));

    /* If v is not assigned to a pCPU, or is not running, no need to bother */
    if ( curr_on_cpu(v->processor) == v )
        cpu_raise_softirq(v->processor, SCHEDULE_SOFTIRQ);

    SCHED_STAT_CRANK(vcpu_sleep);
}

const struct scheduler sched_trivial_def =
        {
                .name           = "Trivial Round Robin Scheduler",
                .opt_name       = "trivial",
                .sched_id       = XEN_SCHEDULER_CREDIT2,
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

                .alloc_pdata    = trivial_alloc_pdata,
                .free_pdata     = trivial_free_pdata,
                .pick_cpu       = trivial_cpu_pick,
                .wake           = trivial_vcpu_wake,
                .sleep          = trivial_vcpu_sleep,
                
                .insert_vcpu = trivial_insert_vcpu,
                .remove_vcpu = trivial_remove_vcpu,
                .do_schedule = trivial_schedule,



      };
REGISTER_SCHEDULER(sched_trivial_def);

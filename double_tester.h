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
#define TWO_PWR(x) (1<<(x))
typedef struct {
int x;
int y;
}db;

s_time_t gcd(s_time_t  a, s_time_t b)
{
s_time_t temp;
  if(a<b)
 {
   temp = a; 
   a=b; 
   b=temp;
 }
 while(b!=0)
   { temp = a%b;
      a = b;
     b = temp;
   }
   return a;
}

void reduce(db *a)
{
  int g = gcd(a->x, a->y);
  a->x = a->x/g;
  a->y = a->y/g;
}

void add(db *a, db *b, db *result)
{
result->y = (a->y)*(b->y);
result->x = (a->x*b->y)+(b->x*a->y);
reduce(result); /* to bring it down to simpler result, if numerator and 
denominator are furhter divisible with one another */
}

void sub(db *a, db *b, db *result)
{
result->x = (a->x*b->y)-(b->x*a->y);
result->y = a->y*b->y;
reduce(result);
}

void mult(db *a, db *b, db * result)
{
result->y = a->y*b->y;
result->x = a->x*b->x;
reduce(result);
}

void div(db *a, db *b, db *result)
{
result->y = a->y*b->x;
result->x = a->x*b->y;
reduce(result);
}


int equal(db *a,db *b)
{
if(a->x*b->y == b->x*a->y)
	return 0;
else
	return -1;
}

void assign(db *a, db *b)
{
a->x = b->x;
a->y = b->y;
}

void ln(db *in, db *result)
{
     db num1,num2,frac,denom,term,sum,old_num,temp;
     old_num.x = 0; 
     old_num.y = 1;
     num1.y = in->y;
     num2.y =  in->y;
     num1.x = in->x - in->y;
     num2.x = in->x + in->y;
     div(&num1, &num2, &num1);
     mult(&num1,&num1, &num2);
     denom.x = denom.y = 1;
     assign(&frac, &num1);
     assign(&term, &frac);
     assign(&sum, &term);
     while(!equal(&sum, &old_num))
     {
       assign(&old_num, &sum);
       denom.x += 2*denom.y;
       mult(&frac, &num2, &frac);
       div(&frac, &num2, &frac);
       add(&sum, &temp, &sum);
       printk("Sum : %d/%d\n",sum.x, sum.y);
       printk("Old Number: %d/%d\n",old_num.x, old_num.y);
     }
    temp.x =2; 
    temp.y = 1;
    mult(&sum,&temp,result);
}

/* db num fed as argument ie availability factor of partition */
db number_calc(db num, int k)
{
    db result,num1,result1,result2,result_final,result3,result4;
    int x=1,y=2;
    num1.x = x;
    num1.y = y;
    /*ln(&num1,&result);*/ /* calculates ln(10) */
    ln(&num,&result); /* calculates ln(alpha) */
    ln(&num1,&result1); /*calculates ln(0.5) */
    div(&result,&result1,&result2); /* caluculates ln(alpha)/ln(0.5) */
    int p=1;
    db num3;
    num3.x=p;
    int number;
    if(num.x>0 && num.y>0 && num.x<num.y &&  k ==1) /* If alpha>0 and k=1 */
    {
    	/* Applying a floor function to get the lower bound */
    	number = (int)(result2.x/result2.y);
    	num3.y = TWO_PWR(number);
    	return(num3);
    }

    else
    {
      	/* Applying a ceil function to get an upper bound */
       	number = (int)((result2.x/result2.y)+1);
      	num3.y = TWO_PWR(number);
    	sub(&num,&num3,&result3);
      	result4 = number_calc(result3,k-1);
      	add(&result4,&num3,&result_final);
     	return(result_final);/* num3 has 1 in numerator and 2^(n) in the denominator */
    }
}


/*
int main()
{
db x_in,result;
x_in.x =1;
x_in.y =2;
ln(&x_in,&result);
return 0;
}
*/

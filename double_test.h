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
#define MAX_INT 100000
typedef struct {
s_time_t x;
s_time_t y;
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

 if(a->x == 0)
 {
	a->y = 1;
	return;
  }

  s_time_t g = gcd(a->x, a->y);
  a->x = a->x/g;
  a->y = a->y/g;
	
    s_time_t absx = a->x>0?a->x:-a->x;
    s_time_t absy = a->y>0?a->y:-a->y;
    int sign = 1;
   while(absx>=MAX_INT || absy>= MAX_INT)
   {
	if(absy>=  MAX_INT)
	{
		if(absy%2!=0)
			a->y -= sign;
		if(absx%2!=0)
			a->x += sign;
          }
	else if(absx>= MAX_INT)
	{
		if(absx%2!=0)
			a->x += sign;
		if(absy%2!=0 && a->y-sign!=0)
			a->y -=sign;
	}
   sign = -sign;
   g= gcd(a->x, a->y);
   a->x /=g;
   a->y /=g;
   absx = a->x>0?a->x:-a->x;
   absy = a->y>0?a->y:-a->y;
   }
   if(a->y<0)
   {
	a->x = - a->x;
	a->y = - a->y;
   }

}

void add(db *a, db *b, db *result)
{
	long long ax = a->x, ay = a->y, bx=b->x, by=b->y;
	result->y = (ay)*(by);
	result->x = (ax*by)+(bx*ay);
	reduce(result); /* to bring it down to simpler result, if numerator and 
denominator are furhter divisible with one another */
}

void minus(db *a, db *b, db *result)
{
	long long ax = a->x, ay = a->y, bx = b->x, by = b->y;
	result->x = (ax*by)-(bx*ay);
	result->y = ay*by;
	reduce(result);
}

void mult(db *a, db *b, db * result)
{
	long long ax = a->x, ay = a->y, bx = b->x, by = b->y;
	result->y = ay*by;
	result->x = ax*bx;
	reduce(result);
}

void division(db *a, db *b, db *result)
{
	long long ax = a->x, ay = a->y, bx = b->x, by = b->y;
	result->y = ay*bx;
	result->x = ax*by;
	reduce(result);
}


int equal(db *a,db *b)
{
	db temp;
	minus(a,b, &temp);
	if(temp.x<0)
		minus(b,a,&temp);
	if(temp.x==0 || temp.y/temp.x>1000000)
		return 1;
	return 0;
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
 division(&num1, &num2, &num1);
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
 division(&frac, &num2, &frac);
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
division(&result,&result1,&result2); /* caluculates ln(alpha)/ln(0.5) */
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
	minus(&num,&num3,&result3);
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

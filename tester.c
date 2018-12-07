#include <stdio.h>
#include "double_tester.h"

int main()
{

	struct db x_in,result,alpha,num3;
	/* taking a 0.5 alpha */
	alpha.x = 3;
	alpha.y = 10;
	int k;
	k=2;
    num3 = number_calc(alpha,k); // this function returns a struct 
    printf("AAF Numerator: %d\n",num3.x);
    printf("AAF Denominator: %d\n",num3.y);
	double x,re;
	x=0.5;
	x_in.x = 1;
	x_in.y = 2;
	ln(&x_in,&result);
	re = result.x/(double)result.y;
	printf("Result is:%f\n",re);
}
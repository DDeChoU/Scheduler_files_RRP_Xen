#include <stdio.h>
#define TWO_PWR(x) (1<<(x))

struct db
{
	/*The result double re = x/y*/
	int x;
	int y;
};
int gcd(int a, int b)
{
	int temp;
	if(a<b)
	{
		temp = a;
		a = b;
		b = temp;
	}
	while(b!=0)
	{
		temp = a%b;
		a = b;
		b = temp;
	}
	return a;
}
void reduce(struct db* a)
{
	int g = gcd(a->x, a->y);
	a->x /= g;
	a->y /= g;
}

void add(struct db* a, struct db* b, struct db* result)
{
	result->y = a->y*b->y;
	result->x = a->x*b->y + b->x*a->y;
	//reduce(result);
}


void minus(struct db*a, struct db* b, struct db* result)
{
	result->y = a->y*b->y;
	result->x = a->x*b->y - b->x*a->y;
	//reduce(result);
}

void mult(struct db*a, struct db*b, struct db* result)
{
	result->y = a->y*b->y;
	result->x = a->x*b->x;
	//reduce(result);
}

void div(struct db*a, struct db*b, struct db* result)
{
	result->y = a->y*b->x;
	result->x = a->x*b->y;
	//reduce(result);
}

/*
int equal(struct db*a, struct db*b)
{
	struct db temp;
	minus(a,b,&temp);
	if(temp.x<0)
		minus(b,a,&temp);
	if(temp.y/temp.x>10000000)
		return 1;
}
*/

int equal(struct db *a, struct db *b)
{
	if(a->x*b->y == b->x*a->y)
		return 0;
	else 
		return -1;
}

void assign(struct db*a, struct db*b)
{
	a->x = b->x;
	a->y = b->y;
}

void ln(struct db* in, struct db* result)
{
	struct db num1, num2, frac, denom, term, sum, old_num, temp;
	old_num.x = 0; old_num.y = 1;
	num1.y = in->y; num2.y = in->y;
	num1.x = in->x - in->y; num2.x = in->x + in->y;
	div(&num1, &num2, &num1);//calculate (x-1)/(x+1) and put that into num1
	mult(&num1, &num1, &num2);
	denom.x = denom.y = 1;
	assign(&frac, &num1);
	assign(&term, &frac);
	assign(&sum, &term);

	while(!equal(&sum, &old_num))
	{
		assign(&old_num, &sum);
		denom.x += 2*denom.y;
		mult(&frac, &num2, &frac);
		//printf("%d/%d\n", frac.x, frac.y);
		div(&frac,&denom, &temp);
		add(&sum, &temp, &sum);
		printf("%d/%d\n", sum.x, sum.y);
		printf("%d/%d\n", old_num.x, old_num.y);
	}
	temp.x = 2; temp.y = 1;
	mult(&sum, &temp, result);

}


double ln_in_double(double x)
{
    double old_sum=0.0;
    double number1 = (x-1)/(x+1);
    double number_2 = number1*number1;
    double denom = 1.0;
    double frac = number1;
    double term = frac;
    double sum = term;

    while(sum!= old_sum)
    {
    old_sum=sum;
    denom+=2.0;
    frac*=number_2;
    //printf("%f\n", frac);
    sum+= frac/denom;
    //printf("%f\n", denom);
    }
    return 2.0*sum;
}

struct db number_calc(struct db num, int k)
{
struct db result,num1,result1,result2,result_final;
int x=1,y=2;
num1.x = x;
num1.y = y;
/*ln(&num1,&result);*/ /* calculates ln(10) */
ln(&num,&result); /* calculates ln(alpha) */
printf("The result is %d\n",(result.y));
ln(&num1,&result1); /*calculates ln(0.5) */
printf("The result1 is %d\n",(result1.x));
div(&result,&result1,&result2); /* caluculates ln(alpha)/ln(0.5) */
 printf("The result2 Denom is %d\n",(result2.y));
 printf("The result2 Numerator is %d\n",(result2.x));
int p=1;
struct db num3;
num3.x=p;
float number;
//if(num.x == 0) //checks for alpha equals 0
//	free(num); 

if(num.x>0 && num.y>0 && num.x<num.y && k==1) //Tests for alpha between 0 and 1
	// K condition can be passed as an argument (if needed)
{
number = (result2.x/(double)result2.y);
printf("The number is: %f\n",number);
num3.y = TWO_PWR((int)number);
return(num3);
}

else
{
	number = ((result2.x)/(double)result2.y)+1; //mimicking ceil by adding 1 to the result
	num3.y = TWO_PWR((int)number);
	return(num3);
}
}

/*
int main()
{
	double x, re;
	struct db x_in,result;
	x = 0.5;
	x_in.x = 1; x_in.y = 2;
	printf("%f\n", ln_in_double(x));
	ln(&x_in, &result);
	re = result.x/(double)result.y;
	printf("%f\n", re);

	return 0;
}
*/

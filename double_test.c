#include <stdio.h>
#define MAX_INT 100000

struct db
{
	/*The result double re = x/y*/
	long x;
	long y;
};
long gcd(long a, long b)
{
	long temp;
	if(a<b)
	{
		temp = a;
		a = b;
		b = temp;
	}
	while(b!=0)
	{
		temp = a%b;
		//printf("Temp now is: %d", temp);
		a = b;
		b = temp;
	}
	//printf("gcd is: (%d)\n", a);
	return a;
}
/*
	Keep integers smaller than a certain value in function reduce
	keep denominators positive.
*/
void reduce(struct db* a)
{
	if(a->x==0)
	{
		a->y = 1;
		return;
	}
	long g = gcd(a->x, a->y);
	a->x /= g;
	a->y /= g;

	int absx = a->x>0?a->x:-a->x;
	int absy = a->y>0?a->y:-a->y;
	int sign = 1;
	while(absx>=MAX_INT || absy >= MAX_INT)
	{
		if(absy>=MAX_INT)
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
			if(absy%2!=0&& a->y-sign!=0)
				a->y -= sign;
		}

		sign = - sign;
		g = gcd(a->x, a->y);
		a->x /= g;
		a->y /= g;
		absx = a->x>0?a->x:-a->x;
		absy = a->y>0?a->y:-a->y;
	}
	if(a->y<0)
	{
		a->x = - a->x;
		a->y = - a->y;
		
	}
	//printf("After number control, the fraction is now: %d, %d\n", a->x, a->y);
	//while(absx)

	//printf("After reduction, the fraction is now: %d, %d\n", a->x, a->y);
}

void add(struct db* a, struct db* b, struct db* result)
{
	long ax = a->x, ay = a->y, bx = b->x, by = b->y;
	result->y = ay*by;
	result->x = ax*by + bx*ay;
	reduce(result);
}


void minus(struct db*a, struct db* b, struct db* result)
{
	long ax = a->x, ay = a->y, bx = b->x, by = b->y;
	result->y = ay*by;
	result->x = ax*by - bx*ay;
	reduce(result);
}

void mult(struct db*a, struct db*b, struct db* result)
{
	long ax = a->x, ay = a->y, bx = b->x, by = b->y;
	result->y = ay*by;
	result->x = ax*bx;
	reduce(result);
}

void div(struct db*a, struct db*b, struct db* result)
{
	long ax = a->x, ay = a->y, bx = b->x, by = b->y;
	result->y = ay*bx;
	result->x = ax*by;
	reduce(result);
}

int equal(struct db*a, struct db*b)
{
	//printf("Comparing (%d,%d) and (%d, %d)\n", a->x, a->y,b->x, b->y);
	struct db temp;
	minus(a,b,&temp);
	if(temp.x<0)
		minus(b,a,&temp);
	if(temp.x==0 || temp.y/temp.x>1000000 )
		return 1;
	return 0;
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

	while(equal(&sum, &old_num)==0)
	{
		//printf("No Exception yet!");
		assign(&old_num, &sum);
		denom.x += 2*denom.y;
		mult(&frac, &num2, &frac);

		double print_d = (double)frac.x/frac.y;
		printf("frac is: %f****", print_d);

		printf("frac is: %d/%d, ", frac.x, frac.y);
		div(&frac,&denom, &temp);
		add(&sum, &temp, &sum);

		print_d = (double)sum.x/sum.y;
		printf("sum is: %f****", print_d);
		printf("sum is: %d/%d\n", sum.x, sum.y);
		getchar();
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
	    printf("frac is: %f, ", frac);
	    sum+= frac/denom;
	    printf("sum is: %f\n", sum);
    }
    return 2.0*sum;
}

int main()
{
	double x, re;
	struct db x_in,result;
	x = 0.09630963;
	x_in.x = 107; x_in.y = 1111;
	printf("%f\n", ln_in_double(x));
	ln(&x_in, &result);
	re = result.x/(double)result.y;
	printf("%f\n", re);
	return 0;
}
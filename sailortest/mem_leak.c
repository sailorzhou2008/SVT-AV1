#include <stdlib.h>

struct elem
{
	int a;
	double b;
	void* c;
};

int main()
{
 	char *p = malloc(sizeof(char) * 10);
	if (p == NULL) {
       return 0;
    }
   
    struct elem *e = malloc(sizeof(struct elem));
    if (e == NULL) {
         free(p);
         return 0;
    }
  
    e->a = 10;
    e->b = 10.10;
    e->c = p;
  
    double *xx = &e->b;
  
    printf("%f\n", *xx);
  
    free(e);
 
    printf("%f\n", *xx);
  
    return 0;
}
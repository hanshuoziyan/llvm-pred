#include "../src/libtiming.c"

#define REPEAT 10
/* use a template to generate instruction */
int inst_template(const char* templ);

int main()
{
   uint64_t t_res = timing_res();
   uint64_t t_err = timing_err();
   uint64_t beg, end, sum = 0, ref = 0;
   for(int i=0; i<REPEAT; ++i){
      beg = timing();
      ref += inst_template("fix_add");
      end = timing();
      sum += end-beg-t_err;
   }
   sum /= 10;
   printf("ref:%lu\n", ref);
   printf("%lu\n", sum);
}
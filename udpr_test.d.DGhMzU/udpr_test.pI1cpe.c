#include <stdio.h>
#include <math.h>
#define HUMAN_READABLE_SIZES       { ' ', 'K', 'M', 'G', 'T', 'P', 'E' }
#define HUMAN_READABLE_SIZES_COUNT 7
static double int_to_human_scale(double value, int *count_out) {
    int count = 0;
    while (value >= 1000 && count < (HUMAN_READABLE_SIZES_COUNT - 1)) { value /= 1000; count++; }
    *count_out = count; return value;
}
double int_to_human_value(double value) { int c; return int_to_human_scale(value, &c); }
char int_to_human_char(double value) {
    static const char s[] = HUMAN_READABLE_SIZES;
    int c; int_to_human_scale(value, &c); return s[c];
}
static int check(double in, double ev, char ec) {
    double gv = int_to_human_value(in); char gc = int_to_human_char(in);
    if (fabs(gv - ev) > 0.0001 || gc != ec) {
        fprintf(stderr, "MISMATCH %.0f: got %.4f'%c', want %.4f'%c'\n", in, gv, gc, ev, ec);
        return 1;
    }
    return 0;
}
int main(void) {
    int f = 0;
    f |= check(0, 0.0, ' ');       f |= check(999, 999.0, ' ');
    f |= check(1000, 1.0, 'K');    f |= check(1001, 1.001, 'K');
    f |= check(1500, 1.5, 'K');    f |= check(1000000, 1.0, 'M');
    f |= check(1500000, 1.5, 'M'); f |= check(1000000000, 1.0, 'G');
    return f;
}

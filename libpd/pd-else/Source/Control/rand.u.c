// porres 2022-2026

#include "m_pd.h"
#include "random.h"
#include <stdlib.h>
#include <string.h>

typedef struct _rand_u{
    t_object       x_obj;
    int            x_id;
    int            x_offset;
    int            x_size;
    int           *x_occurances;
    int           *x_candidates;
    int            x_n_candidates;
    t_random_state x_rstate;
    int           *x_hold_v;      // ring buffer hold values
    int            x_hold_head;   // write position in ring buffer
    int            x_hold_n;      // number of values to hold
    t_outlet      *x_bang_out;
}t_rand_u;

static t_class *rand_u_class;

static void rand_u_seed(t_rand_u *x, t_symbol *s, int ac, t_atom *av){
    random_init(&x->x_rstate, get_seed(s, ac, av, x->x_id));
}

static void rand_u_bang(t_rand_u *x){
    x->x_n_candidates = 0;
    for(int i = 0; i < x->x_size; i++)
        x->x_n_candidates += (!x->x_occurances[i]);
    x->x_candidates = (int*)getbytes(x->x_n_candidates*sizeof(int));
    for(int i = 0, j = 0; i < x->x_size; i++){
        if(!x->x_occurances[i])
            *(x->x_candidates+j++) = i;
    }
    uint32_t *s1 = &x->x_rstate.s1, *s2 = &x->x_rstate.s2, *s3 = &x->x_rstate.s3;
    t_float noise = (t_float)(random_frand(s1, s2, s3)) * 0.5 + 0.5;
    int n = (int)(noise * x->x_n_candidates);
    if(n >= x->x_n_candidates)
        n = x->x_n_candidates-1;
    int idx = *(x->x_candidates+n);
    outlet_float(x->x_obj.ob_outlet, idx + x->x_offset);
    *(x->x_occurances+idx) += 1;
    // track hold values
    if(x->x_hold_n > 0){
        memmove(x->x_hold_v, x->x_hold_v+1, (x->x_hold_n-1)*sizeof(int));
        x->x_hold_v[x->x_hold_n-1] = idx;
    }
    if(x->x_n_candidates == 1){ // end of set
        outlet_bang(x->x_bang_out);
        memset(x->x_occurances, 0x0, x->x_size*sizeof(int));
        for(int i = 0; i < x->x_hold_n; i++)
            x->x_occurances[x->x_hold_v[i]] = 1;
    }
    freebytes(x->x_candidates, x->x_n_candidates*sizeof(int));
}

static void rand_u_hold(t_rand_u *x, t_float f){
    int limit = x->x_size / 2;
    int bufsize = f > limit ? limit : f < 0 ? 0 : (int)f;
    if(bufsize != x->x_hold_n){
        if(x->x_hold_v)
            freebytes(x->x_hold_v, x->x_hold_n*sizeof(int));
        x->x_hold_n = bufsize;
        if(x->x_hold_n > 0){
            x->x_hold_v = (int*)getbytes(x->x_hold_n*sizeof(int));
            memset(x->x_hold_v, 0x0, x->x_hold_n*sizeof(int));
        }
        else
            x->x_hold_v = NULL;
        x->x_hold_head = 0;
        // also clear occurances
        memset(x->x_occurances, 0x0, x->x_size*sizeof(int));
    }
}

static void rand_u_offset(t_rand_u *x, t_float f){
    x->x_offset = (int)f;
}

static void rand_u_size(t_rand_u *x, t_float f){
    int n = f < 1 ? 1 : (int)f;
    if(n != x->x_size){ // resize
        freebytes(x->x_occurances, x->x_size*sizeof(int));
        x->x_size = n;
        x->x_occurances = (int*)getbytes(x->x_size*sizeof(int));
        memset(x->x_occurances, 0x0, x->x_size*sizeof(int));
        // recompute carry for new size
        if(x->x_hold_v)
            freebytes(x->x_hold_v, x->x_hold_n*sizeof(int));
        int limit = x->x_size / 2;
        if(x->x_hold_n > limit)
            x->x_hold_n = limit;
        if(x->x_hold_n > 0){
            x->x_hold_v = (int*)getbytes(x->x_hold_n*sizeof(int));
            memset(x->x_hold_v, 0x0, x->x_hold_n*sizeof(int));
        }
        else
            x->x_hold_v = NULL;
        x->x_hold_head = 0;
    }
}

static void rand_u_clear(t_rand_u *x){
    memset(x->x_occurances, 0x0, x->x_size*sizeof(int));
    if(x->x_hold_v && x->x_hold_n > 0)
        memset(x->x_hold_v, 0x0, x->x_hold_n*sizeof(int));
    x->x_hold_head = 0;
}

static t_rand_u *rand_u_new(t_symbol *s, int ac, t_atom *av){
    (void)s;
    t_rand_u *x = (t_rand_u *)pd_new(rand_u_class);
    x->x_id = random_get_id();
    rand_u_seed(x, s, 0, NULL);
    x->x_occurances = NULL;
    x->x_candidates = NULL;  
    x->x_size = 2;
    x->x_offset = 0;
    x->x_hold_n = 0;
    x->x_hold_v = NULL;
    x->x_hold_head = 0;
    while(ac && av[0].a_type == A_SYMBOL){
        if(av[0].a_w.w_symbol == gensym("-seed")){
            if(ac >= 2){
                if(av[1].a_type == A_FLOAT){
                    t_atom at[1];
                    SETFLOAT(at, atom_getfloat(av+1));
                    ac-=2, av+=2;
                    rand_u_seed(x, s, 1, at);
                }
                else
                    goto errstate;
            }
            else
                goto errstate;
        }
        else if(av[0].a_w.w_symbol == gensym("-offset")){
            if(ac >= 2){
                ac--, av++;
                if(av->a_type == A_FLOAT){
                    x->x_offset = atom_getint(av);
                    ac--, av++;
                }
                else
                    goto errstate;
            }
            else
                goto errstate;
        }
        else
            goto errstate;
    }
    if(ac && av[0].a_type == A_FLOAT){
        int size = (int)atom_getfloat(av);
        x->x_size = size <= 0 ? 2 : size;
        ac--, av++;
    }
    if(ac && av[0].a_type == A_FLOAT){
        int hold = atom_getint(av);
        int limit = x->x_size / 2;
        x->x_hold_n = hold > limit ? limit : hold < 0 ? 0 : hold;
        ac--, av++;
    }
    x->x_occurances = (int*)getbytes(x->x_size*sizeof(int));
    memset(x->x_occurances, 0x0, x->x_size*sizeof(int));
    if(x->x_hold_n > 0){
        x->x_hold_v = (int*)getbytes(x->x_hold_n*sizeof(int));
        memset(x->x_hold_v, 0x0, x->x_hold_n*sizeof(int));
    }
    inlet_new((t_object *)x, (t_pd *)x, &s_float, gensym("size"));
    outlet_new(&x->x_obj, &s_float);
    x->x_bang_out = outlet_new(&x->x_obj, &s_bang);
    return(x);
errstate:
    post("[rand.u] improper args");
    return(NULL);
}

static void rand_u_free(t_rand_u *x){
    if(x->x_occurances)
        freebytes(x->x_occurances, x->x_size*sizeof(int));
    if(x->x_hold_v && x->x_hold_n > 0)
        freebytes(x->x_hold_v, x->x_hold_n*sizeof(int));
}

void setup_rand0x2eu(void){
    rand_u_class = class_new(gensym("rand.u"), (t_newmethod)(void*)rand_u_new,
        (t_method)rand_u_free, sizeof(t_rand_u), 0, A_GIMME, 0);
    class_addbang(rand_u_class, rand_u_bang);
    class_addmethod(rand_u_class, (t_method)rand_u_offset, gensym("offset"), A_FLOAT, 0);
    class_addmethod(rand_u_class, (t_method)rand_u_size, gensym("size"), A_FLOAT, 0);
    class_addmethod(rand_u_class, (t_method)rand_u_hold, gensym("hold"), A_FLOAT, 0);
    class_addmethod(rand_u_class, (t_method)rand_u_seed, gensym("seed"), A_GIMME, 0);
    class_addmethod(rand_u_class, (t_method)rand_u_clear, gensym("clear"), 0);
}

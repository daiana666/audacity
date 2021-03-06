/* trigger.c -- return zero until input transitions from <=0 to >0, then
   evaluate a closure to get a signal and convert to an add
   of the new signal and a copy of this trigger object. 
   The sample rate of the output is the sample rate of the input, and 
   sounds returned by the closure must also have a matching sample rate.
   The trigger will take place on the first input sample (zero delay) if the
   first sample of the input is >0.
   The input scale factor is assumed to be 1, so caller should force scaling
   especially if the scale factor is negative (!)
   The trigger terminates when the input signal terminates (but any adds
   continue to run until all their inputs terminate).

Some implementation notes:

The closure gets evaluated at the time of the positive sample.
When the positive sample is encountered, first close off the
current output block. 

Next, evaluate the closure, clone the trigger, and convert 
the current trigger to an add. The next fetch will therefore
go to the add susp and it will add the closure result to the
zeros that continue to be generated by (a clone of) the trigger.
It would be simple if we could back the clone up one sample:
on the first call to the add, it will ask for samples from the
trigger clone and the closure, but the trigger clone has already
processed the positive sample, so it is one sample ahead of 
everyone else. Since we've just read a sample, we CAN back up
just by carefully updating the input pointer to one less than
we actually read, forcing a reread later. (We'll still store
the previous value so re-reading will not re-trigger.)
*/

/* CHANGE LOG
 * --------------------------------------------------------------------
 * 13Dec06  rbd  created from sndseq.c
 */

#include "stdio.h"
#ifndef mips
#include "stdlib.h"
#endif
#include "xlisp.h"
#include "sound.h"
#include "falloc.h"
#include "scale.h"
#include "add.h"
#include "extern.h"
#include "cext.h"
#include "assert.h"

#define TRIGGERDBG 1
#define D if (TRIGGERDBG) 

/* Note: this structure is identical to an add_susp structure up
   to the field output_per_s2 so that we can convert this into
   an add after eval'ing the closure.  Since this struct is bigger
   than an add, make sure not to clobber the "free" routine 
   (trigger_free) or else we'll leak memory.
 */
typedef struct trigger_susp_struct {
    snd_susp_node               susp;
    boolean                     started;
    int                         terminate_bits;
    int64_t                     terminate_cnt;
    int                         logical_stop_bits;
    boolean                     logically_stopped;
    sound_type                  s1;
    int                         s1_cnt;
    sample_block_type           s1_bptr;        /* block pointer */
    sample_block_values_type    s1_ptr;
    sound_type                  s2;
    int                         s2_cnt;
    sample_block_type           s2_bptr;        /* block pointer */
    sample_block_values_type    s2_ptr;

    /* trigger-specific data starts here */
    sample_type                 previous;
    LVAL                        closure;

} trigger_susp_node, *trigger_susp_type;


void trigger_fetch(snd_susp_type, snd_list_type);
void trigger_free(snd_susp_type a_susp);

extern LVAL s_stdout;

void trigger_mark(snd_susp_type a_susp)
{
    trigger_susp_type susp = (trigger_susp_type) a_susp;
    sound_xlmark(susp->s1);
    if (susp->closure) mark(susp->closure);
}



/* trigger_fetch returns zero blocks until s1 goes from <=0 to >0 */
/**/
void trigger_fetch(snd_susp_type a_susp, snd_list_type snd_list)
{
    trigger_susp_type susp = (trigger_susp_type) a_susp;
    int cnt = 0; /* how many samples computed */
    int togo;
    int n;
    sample_block_type out;
    register sample_block_values_type out_ptr;
    register sample_block_values_type out_ptr_reg;
    register sample_block_values_type input_ptr_reg;
    falloc_sample_block(out, "trigger_fetch");
    out_ptr = out->samples;
    snd_list->block = out;

    while (cnt < max_sample_block_len) { /* outer loop */
        /* first compute how many samples to generate in inner loop: */
        /* don't overflow the output sample block */
        togo = max_sample_block_len - cnt;

        /* don't run past the input sample block: */
        susp_check_term_samples(s1, s1_ptr, s1_cnt);
        togo = min(togo, susp->s1_cnt);

        /* don't run past terminate time */
        if (susp->terminate_cnt != UNKNOWN &&
            susp->terminate_cnt <= susp->susp.current + cnt + togo) {
            togo = (int) (susp->terminate_cnt - (susp->susp.current + cnt));
            if (togo == 0) break;
        }

        n = togo;
        input_ptr_reg = susp->s1_ptr;
        out_ptr_reg = out_ptr;
        if (n) do { /* the inner sample computation loop */
            sample_type s = *input_ptr_reg++;
            if (susp->previous <= 0 && s > 0) {
                trigger_susp_type new_trigger;
                sound_type new_trigger_snd;
                LVAL result;
                int64_t delay; /* sample delay to s2 */
                time_type now;

                susp->previous = s; /* don't retrigger */

                /**** close off block ****/
                togo = togo - n;
                susp->s1_ptr += togo;
                susp_took(s1_cnt, togo);
                cnt += togo;
                snd_list->block_len = cnt;
                susp->susp.current += cnt;
                now = susp->susp.t0 + susp->susp.current / susp->susp.sr;

                /**** eval closure and add result ****/
D               nyquist_printf("trigger_fetch: about to eval closure at %g, "
                               "susp->susp.t0 %g, susp.current %d:\n",
                               now, susp->susp.t0, (int)susp->susp.current);
                xlsave1(result);
                result = xleval(cons(susp->closure, consa(cvflonum(now))));
                if (exttypep(result, a_sound)) {
                    susp->s2 = sound_copy(getsound(result));
D                   nyquist_printf("trigger: copied result from closure is %p\n",
                                   susp->s2);
                } else xlerror("closure did not return a (monophonic) sound", 
                               result);
D               nyquist_printf("in trigger: after evaluation; "
                               "%p returned from evform\n",
                               susp->s2);
                result = NIL;

                /**** cloan this trigger to become s1 ****/
                falloc_generic(new_trigger, trigger_susp_node, 
                               "new_trigger");
                memcpy(new_trigger, susp, sizeof(trigger_susp_node));
                /* don't copy s2 -- it should only be referenced by add */
                new_trigger->s2 = NULL;
                new_trigger_snd = sound_create((snd_susp_type) new_trigger,
                                               now, susp->susp.sr, 1.0F);
                susp->s1 = new_trigger_snd;
                /* add will have to ask new_trigger for samples, new_trigger
                 * will continue reading samples from s1 (the original input)
                 */
                susp->s1_cnt = 0;
                susp->s1_ptr = NULL;

                /**** convert to add ****/
                susp->susp.mark = add_mark;
                /* logical stop will be recomputed by add: */
                susp->susp.log_stop_cnt = UNKNOWN; 
                susp->susp.print_tree = add_print_tree;

                /* assume sample rates are the same */
                if (susp->s1->sr != susp->s2->sr) 
                    xlfail("in trigger: sample rates must match");

                /* take care of scale factor, if any */
                if (susp->s2->scale != 1.0) {
                    // stdputstr("normalizing next sound in a seq\n");
                    susp->s2 = snd_make_normalize(susp->s2);
                }

                /* figure out which add fetch routine to use */
                delay = ROUNDBIG((susp->s2->t0 - now) * susp->s1->sr);
                if (delay > 0) {    /* fill hole between s1 and s2 */
                    D stdputstr("using add_s1_nn_fetch\n");
                    susp->susp.fetch = add_s1_nn_fetch;
                    susp->susp.name = "trigger:add_s1_nn_fetch";
                } else {
                    susp->susp.fetch = add_s1_s2_nn_fetch;
                    susp->susp.name = "trigger:add_s1_s2_nn_fetch";
                }

D               stdputstr("in trigger: calling add's fetch\n");
                /* fetch will get called later ..
                   (*(susp->susp.fetch))(a_susp, snd_list); */
D               stdputstr("in trigger: returned from add's fetch\n");
                xlpop();

                susp->closure = NULL;   /* allow garbage collection now */
                /**** calculation tree modified, time to exit ****/
                /* but if cnt == 0, then we haven't computed any samples */
                /* call on new fetch routine to get some samples */
                if (cnt == 0) {
                    // because adder will reallocate
                    ffree_sample_block(out, "trigger-pre-adder");
                    (*susp->susp.fetch)(a_susp, snd_list);
                }
                return;
            } else {
                susp->previous = s;
                /* output zero until ready to add in closure */
                *out_ptr_reg++ = 0; 
            }
        } while (--n); /* inner loop */

        /* using input_ptr_reg is a bad idea on RS/6000: */
        susp->s1_ptr += togo;
        out_ptr += togo;
        susp_took(s1_cnt, togo);
        cnt += togo;
    } /* outer loop */

    if (togo == 0 && cnt == 0) {
        snd_list_terminate(snd_list);
    } else {
        snd_list->block_len = cnt;
        susp->susp.current += cnt;
    }
} /* trigger_fetch */


void trigger_free(snd_susp_type a_susp)
{
    trigger_susp_type susp = (trigger_susp_type) a_susp;
    sound_unref(susp->s1);
    sound_unref(susp->s2);
    ffree_generic(susp, sizeof(trigger_susp_node), "trigger_free");
}


void trigger_print_tree(snd_susp_type a_susp, int n)
{
    trigger_susp_type susp = (trigger_susp_type) a_susp;
    indent(n);
    stdputstr("s1:");
    sound_print_tree_1(susp->s1, n);

    indent(n);
    stdputstr("closure:");
    stdprint(susp->closure);

    indent(n);
    stdputstr("s2:");
    sound_print_tree_1(susp->s2, n);
}




sound_type snd_make_trigger(sound_type s1, LVAL closure)
{
    register trigger_susp_type susp;
    /* t0 specified as input parameter */
    sample_type scale_factor = 1.0F;
    sound_type result;

    xlprot1(closure);
    falloc_generic(susp, trigger_susp_node, "snd_make_trigger");

    if (s1->scale != 1.0) {
        /* stdputstr("normalizing first sound in a seq\n"); */
        s1 = snd_make_normalize(s1);
    }

    susp->susp.fetch = trigger_fetch;

    susp->terminate_cnt = UNKNOWN;
    susp->terminate_bits = 0;   /* bits for s1 and s2 termination */
    susp->logical_stop_bits = 0;    /* bits for s1 and s2 logical stop */

    /* initialize susp state */
    susp->susp.free = trigger_free;
    susp->susp.sr = s1->sr;
    susp->susp.t0 = s1->t0;
    susp->susp.mark = trigger_mark;
    susp->susp.print_tree = trigger_print_tree;
    susp->susp.name = "trigger";
    susp->logically_stopped = false;
    susp->susp.log_stop_cnt = s1->logical_stop_cnt;
    susp->started = false;
    susp->susp.current = 0;
    susp->s1 = s1;
    susp->s1_cnt = 0;
    susp->s2 = NULL;
    susp->s2_cnt = 0;
    susp->closure = closure;
    susp->previous = 0;
    result = sound_create((snd_susp_type)susp, susp->susp.t0, susp->susp.sr, scale_factor);
    xlpopn(1);
    return result;
}


sound_type snd_trigger(sound_type s1, LVAL closure)
{
    sound_type s1_copy;
    s1_copy = sound_copy(s1);
    return snd_make_trigger(s1_copy, closure);
}

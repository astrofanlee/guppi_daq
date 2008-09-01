/* guppi_psrfits_thread.c
 *
 * Write databuf blocks out to disk.
 */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>

#include "fitshead.h"
#include "polyco.h"
#include "psrfits.h"
#include "fold.h"
#include "guppi_error.h"
#include "guppi_status.h"
#include "guppi_databuf.h"

#define STATUS_KEY "DISKSTAT"
#include "guppi_threads.h"

// Read a status buffer all of the key observation paramters
extern void guppi_read_obs_params(char *buf, 
                                  struct guppi_params *g, 
                                  struct psrfits *p);

/* Parse info from buffer into param struct */
extern void guppi_read_subint_params(char *buf, 
                                     struct guppi_params *g,
                                     struct psrfits *p);

/* Downsampling functions */
extern void get_stokes_I(struct psrfits *pf);
extern void downsample_freq(struct psrfits *pf);
extern void downsample_time(struct psrfits *pf);
extern void guppi_update_ds_params(struct psrfits *pf);

void zero_end_chans(struct psrfits *pf)
{
    int ii, jj;
    struct hdrinfo *hdr = &(pf->hdr);
    char *data = (char *)pf->sub.data;
    const int nchan = hdr->nchan;
    const int nspec = hdr->nsblk * hdr->npol;
    
    for (ii = 0, jj = 0 ; ii < nspec ; ii++, jj += nchan)
        data[jj] = data[jj+nchan-1] = 0;
}


void guppi_psrfits_thread(void *_args) {
    
    /* Set cpu affinity */
    cpu_set_t cpuset, cpuset_orig;
    sched_getaffinity(0, sizeof(cpu_set_t), &cpuset_orig);
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    int rv = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    if (rv<0) { 
        guppi_error("guppi_psrfits_thread", "Error setting cpu affinity.");
        perror("sched_setaffinity");
    }

    /* Get args */
    struct guppi_thread_args *args = (struct guppi_thread_args *)_args;
    
    /* Set priority */
    rv = setpriority(PRIO_PROCESS, 0, 0);
    if (rv<0) {
        guppi_error("guppi_psrfits_thread", "Error setting priority level.");
        perror("set_priority");
    }
    
    /* Attach to status shared mem area */
    struct guppi_status st;
    rv = guppi_status_attach(&st);
    if (rv!=GUPPI_OK) {
        guppi_error("guppi_psrfits_thread", 
                    "Error attaching to status shared memory.");
        pthread_exit(NULL);
    }
    pthread_cleanup_push((void *)set_exit_status, &st);
    
    /* Init status */
    guppi_status_lock_safe(&st);
    hputs(st.buf, STATUS_KEY, "init");
    guppi_status_unlock_safe(&st);
    
    /* Initialize some key parameters */
    struct guppi_params gp;
    struct psrfits pf;
    pf.sub.data = NULL;
    pf.sub.dat_freqs = pf.sub.dat_weights = NULL;
    pf.sub.dat_offsets = pf.sub.dat_scales = NULL;
    pf.filenum = 0; // This is crucial
    pthread_cleanup_push((void *)psrfits_close, &pf);
    
    /* Attach to databuf shared mem */
    struct guppi_databuf *db;
    db = guppi_databuf_attach(args->input_buffer);
    if (db==NULL) {
        guppi_error("guppi_psrfits_thread",
                    "Error attaching to databuf shared memory.");
        pthread_exit(NULL);
    }
    
    /* Loop */
    int curblock=0, total_status=0, firsttime=1, run=1, got_packet_0=0;
    int mode=SEARCH_MODE;
    char *ptr;
    struct foldbuf *fb=NULL;
    float *fold_output_array = NULL;
    signal(SIGINT, cc);
    do {
        /* Note waiting status */
        guppi_status_lock_safe(&st);
        hputs(st.buf, STATUS_KEY, "waiting");
        guppi_status_unlock_safe(&st);
        
        /* Wait for buf to have data */
        guppi_databuf_wait_filled(db, curblock);

        /* Note current block */
        guppi_status_lock_safe(&st);
        hputi4(st.buf, "CURBLOCK", curblock);
        guppi_status_unlock_safe(&st);

        /* See how full databuf is */
        total_status = guppi_databuf_total_status(db);
        
        /* Read param structs for this block */
        ptr = guppi_databuf_header(db, curblock);
        if (firsttime) {
            guppi_read_obs_params(ptr, &gp, &pf);
            firsttime = 0;
        } else {
            guppi_read_subint_params(ptr, &gp, &pf);
        }

        /* Find out what mode this data is in */
        mode = psrfits_obs_mode(pf.hdr.obs_mode);

        /* Check if we got packet 0.  If so, flag writing to 
         * start, and update obs_params as well.
         */
        if (got_packet_0==0 && gp.packetindex==0) {
            got_packet_0 = 1;
            guppi_read_obs_params(ptr, &gp, &pf);
            guppi_update_ds_params(&pf);
        }

        /* If actual observation has started, write the data */
        if (got_packet_0) { 

            /* Note waiting status */
            guppi_status_lock_safe(&st);
            hputs(st.buf, STATUS_KEY, "writing");
            guppi_status_unlock_safe(&st);
            
            /* Get the pointer to the current data */
            if (mode==FOLD_MODE) {
                printf("fold mode\n");
                fb = (struct foldbuf *)guppi_databuf_data(db, curblock);
                fold_output_array = (float *)realloc(fold_output_array,
                        sizeof(float) * pf.hdr.nbin * pf.hdr.nchan * 
                        pf.hdr.npol);
                pf.sub.data = (unsigned char *)fold_output_array;
            } else
                pf.sub.data = (unsigned char *)guppi_databuf_data(db, curblock);
            
            /* Set the DC and Nyquist channels explicitly to zero */
            /* because of the "FFT Problem" that splits DC power  */
            /* into those two bins.                               */
            zero_end_chans(&pf);

            /* Output only Stokes I (in place) */
            if (pf.hdr.onlyI && pf.hdr.npol==4)
                get_stokes_I(&pf);

            /* Downsample in frequency (in place) */
            if (pf.hdr.ds_freq_fact > 1)
                downsample_freq(&pf);

            /* Downsample in time (in place) */
            if (pf.hdr.ds_time_fact > 1)
                downsample_time(&pf);

            /* Folded data needs a transpose */
            if (mode==FOLD_MODE)
                normalize_transpose_folds(fold_output_array, fb);

            /* Write the data */
            psrfits_write_subint(&pf);

            /* Fold mode: deal with polyco, ephemeris table */
            if (mode==FOLD_MODE) {

                if (pf.rownum==2) {
                    printf("remove ephem\n");
                    psrfits_remove_ephem(&pf); // XXX for now
                    struct polyco *pc;
                    FILE *pcf = fopen("polyco.dat","r"); // XXX also temp
                    if (pcf!=NULL) { 
                        int npc = read_all_pc(pcf, &pc);
                        fclose(pcf);
                        if (npc>0) 
                            psrfits_write_polycos(&pf, pc, npc);
                    }
                }

            }
                
            /* Is the scan complete? */
            if ((pf.hdr.scanlen > 0.0) && 
                (pf.T > pf.hdr.scanlen)) run = 0;
            
            /* For debugging... */
            if (gp.drop_frac > 0.0) {
               printf("Block %d dropped %.3g%% of the packets\n", 
                      pf.tot_rows, gp.drop_frac*100.0);
            }

        }

        /* Mark as free */
        guppi_databuf_set_free(db, curblock);
        
        /* Go to next block */
        curblock = (curblock + 1) % db->n_block;
        
        /* Check for cancel */
        pthread_testcancel();
        
    } while (run);
    
    /* Cleanup */
    
    if (fold_output_array!=NULL) free(fold_output_array);
    free(pf.sub.dat_freqs);
    free(pf.sub.dat_weights);
    free(pf.sub.dat_offsets);
    free(pf.sub.dat_scales);
    
    pthread_exit(NULL);
    
    pthread_cleanup_pop(0); /* Closes psrfits_close */
    pthread_cleanup_pop(0); /* Closes set_exit_status */
}

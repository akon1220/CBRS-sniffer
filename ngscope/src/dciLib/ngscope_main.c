#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>

#include "srsran/srsran.h"
#include "ngscope/hdr/dciLib/radio.h"
#include "ngscope/hdr/dciLib/task_scheduler.h"
#include "ngscope/hdr/dciLib/dci_decoder.h"
#include "ngscope/hdr/dciLib/ngscope_def.h"
#include "ngscope/hdr/dciLib/load_config.h"
#include "ngscope/hdr/dciLib/status_tracker.h"

pthread_mutex_t     cell_mutex = PTHREAD_MUTEX_INITIALIZER;
srsran_cell_t       cell_vec[MAX_NOF_RF_DEV];
// {0, 0} means nof_dci, header
dci_ready_t         dci_ready = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

ngscope_status_buffer_t    dci_buffer[MAX_DCI_BUFFER];

int ngscope_main(ngscope_config_t* config, prog_args_t* prog_args){
    //usually the # of USRP (1)
    int nof_rf_dev;

    // Init the cell
    for(int i=0; i<MAX_NOF_RF_DEV; i++){
        // filling with 0
        memset(&cell_vec[i], 0, sizeof(srsran_cell_t));
    }

    nof_rf_dev = config->nof_rf_dev;
    prog_args->nof_rf_dev       = nof_rf_dev;
    prog_args->log_dl           = config->dci_log_config.log_dl;
    prog_args->log_ul           = config->dci_log_config.log_ul;
    prog_args->rnti             = (uint16_t)config->rnti;
    prog_args->remote_enable    = config->remote_enable;
//    prog_args->rf_nof_rx_ant    = config->rf_nof_rx_ant;

    // this is 1 when we use 1 USRP.
    printf("number of decoder: %d", config->rf_config[0].nof_thread);
    /* Task scheduler thread */
    // the most important part? 
    pthread_t task_thd[MAX_NOF_RF_DEV];
    for(int i=0; i<nof_rf_dev; i++){
        // values from rf_dev_config_t structu 
        prog_args->rf_index      = i;
        prog_args->rf_freq       = config->rf_config[i].rf_freq;
        prog_args->rf_freq_vec[i]= config->rf_config[i].rf_freq;

        prog_args->force_N_id_2  = config->rf_config[i].N_id_2;
        prog_args->nof_decoder   = config->rf_config[i].nof_thread;
        prog_args->disable_plots = config->rf_config[i].disable_plot;

        //prog_args->rf_nof_rx_ant = config->rf_config[i].rf_nof_rx_ant;
        
        prog_args->rf_args    = (char*) malloc(100 * sizeof(char));
        strcpy(prog_args->rf_args, config->rf_config[i].rf_args);

//        printf("rx antenna: %d \n", prog_args->rf_nof_rx_ant);

        //call task_scheduler_thread function from task_scheduler_thread.c
        pthread_create(&task_thd[i], NULL, task_scheduler_thread, (void*)(prog_args));
        printf("task thread was called \n");
    }
    printf("no more task thread to be called \n");

    pthread_t status_thd;
    prog_args->disable_plots    = config->rf_config[0].disable_plot;
    printf("disable_plots :%d\n", prog_args->disable_plots);

    pthread_create(&status_thd, NULL, status_tracker_thread, (void*)(prog_args));
    printf("status thread was called \n");
    /* Now waiting for those threads to end */
    for(int i=0; i<nof_rf_dev; i++){
        printf("waiting for task thread to be done \n");
        pthread_join(task_thd[i], NULL);
    }
    printf("waiting for status thread to be done \n");
    pthread_join(status_thd, NULL);
    return 1;
}

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

#include "ngscope/hdr/dciLib/radio.h"
#include "ngscope/hdr/dciLib/task_scheduler.h"
#include "ngscope/hdr/dciLib/dci_decoder.h"
#include "ngscope/hdr/dciLib/ngscope_def.h"
#include "ngscope/hdr/dciLib/phich_decoder.h"

extern bool go_exit;

extern pthread_mutex_t     cell_mutex; 
extern srsran_cell_t       cell_vec[MAX_NOF_RF_DEV];

extern dci_ready_t              dci_ready;
extern ngscope_status_buffer_t  dci_buffer[MAX_DCI_BUFFER];

/******************* Global buffer for passing subframe IQ  ******************/ 
ngscope_sf_buffer_t sf_buffer[MAX_NOF_DCI_DECODER] = 
{
    {0, 0, {NULL}, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER},
    {0, 0, {NULL}, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER},
    {0, 0, {NULL}, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER},
    {0, 0, {NULL}, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER},
};
bool                sf_token[MAX_NOF_DCI_DECODER];
pthread_mutex_t     token_mutex = PTHREAD_MUTEX_INITIALIZER;

pend_ack_list       ack_list;
pthread_mutex_t     ack_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t pkt_errors = 0, pkt_total = 0;

int find_idle_decoder(int nof_decoder){
    int idle_idx = -1;
    pthread_mutex_lock(&token_mutex);
    for(int i=0;i<nof_decoder;i++){
        if(sf_token[i] == false){       
            idle_idx = i;
            break;
        } 
    } 
    pthread_mutex_unlock(&token_mutex);
    return idle_idx;
}
/*****************************************************************************/


/********************** callback wrapper **********************/ 
int srsran_rf_recv_wrapper(void* h, cf_t* data_[SRSRAN_MAX_PORTS], uint32_t nsamples, srsran_timestamp_t* t)
{
  DEBUG(" ----  Receive %d samples  ----", nsamples);
  void* ptr[SRSRAN_MAX_PORTS];
  for (int i = 0; i < SRSRAN_MAX_PORTS; i++) {
    ptr[i] = data_[i];
  }
  //return srsran_rf_recv_with_time_multi(h, ptr, nsamples, true, NULL, NULL);
  return srsran_rf_recv_with_time_multi(h, ptr, nsamples, true, &t->full_secs, &t->frac_secs);
}

static SRSRAN_AGC_CALLBACK(srsran_rf_set_rx_gain_th_wrapper_)
{
  srsran_rf_set_rx_gain_th((srsran_rf_t*)h, gain_db);
}
/******************** End of callback wrapper ********************/ 

// Init MIB
int mib_init_imp(srsran_ue_mib_t*   ue_mib,
                    cf_t*           sf_buffer[SRSRAN_MAX_PORTS],
                    srsran_cell_t*  cell)
{
    if (srsran_ue_mib_init(ue_mib, sf_buffer[0], cell->nof_prb)) {
        ERROR("Error initaiting UE MIB decoder");
        exit(-1);
    }
    if (srsran_ue_mib_set_cell(ue_mib, *cell)) {
        ERROR("Error initaiting UE MIB decoder");
        exit(-1);
    }
    return SRSRAN_SUCCESS;
}

int ue_mib_decode_sfn(srsran_ue_mib_t*   ue_mib,
                        srsran_cell_t*   cell,
                        uint32_t*        sfn,
                        bool             decode_pdcch)
{
    uint8_t bch_payload[SRSRAN_BCH_PAYLOAD_LEN];
    int     sfn_offset;
    // decode MIB and get information about SFN and SFN offset
    int n = srsran_ue_mib_decode(ue_mib, bch_payload, NULL, &sfn_offset);
    if (n < 0) {
      ERROR("Error decoding UE MIB");
      exit(-1);
    } else if (n == SRSRAN_UE_MIB_FOUND) {
      srsran_pbch_mib_unpack(bch_payload, cell, sfn);
      if(!decode_pdcch){
          srsran_cell_fprint(stdout, cell, *sfn);
          printf("Decoded MIB. SFN: %d, offset: %d\n", *sfn, sfn_offset);
      }
      *sfn   = (*sfn + sfn_offset) % 1024;
    }
    return SRSRAN_SUCCESS;
}

// Initialize UE sync
int ue_sync_init_imp(srsran_ue_sync_t*      ue_sync,
                        srsran_rf_t*        rf, 
                        srsran_cell_t*      cell,
                        cell_search_cfg_t*  cell_detect_config,
                        prog_args_t         prog_args,
                        float               search_cell_cfo)
{
    int decimate = 0;
    if (prog_args.decimate) {
        if (prog_args.decimate > 4 || prog_args.decimate < 0) {
            printf("Invalid decimation factor, setting to 1 \n");
        } else {
            decimate = prog_args.decimate;
        }
    }
    // Init the structure
    if (srsran_ue_sync_init_multi_decim(ue_sync,
                                        cell->nof_prb,
                                        cell->id == 1000,
                                        srsran_rf_recv_wrapper,
                                        prog_args.rf_nof_rx_ant,
                                        (void*)rf,
                                        decimate)) {
      ERROR("Error initiating ue_sync");
      exit(-1);
    }
    // UE sync set cell info
    if (srsran_ue_sync_set_cell(ue_sync, *cell)) {
      ERROR("Error initiating ue_sync");
      exit(-1);
    }

    // Disable CP based CFO estimation during find
    ue_sync->cfo_current_value       = search_cell_cfo / 15000;
    ue_sync->cfo_is_copied           = true;
    ue_sync->cfo_correct_enable_find = true;
    srsran_sync_set_cfo_cp_enable(&ue_sync->sfind, false, 0);
    printf("CFO value: %f \n", ue_sync->cfo_current_value);
    
    // set AGC
    if (prog_args.rf_gain < 0) {
        srsran_rf_info_t* rf_info = srsran_rf_get_info(rf);
        srsran_ue_sync_start_agc(ue_sync,
                             srsran_rf_set_rx_gain_th_wrapper_,
                             rf_info->min_rx_gain,
                             rf_info->max_rx_gain,
                             cell_detect_config->init_agc);
    }
    ue_sync->cfo_correct_enable_track = !prog_args.disable_cfo;
      
    return SRSRAN_SUCCESS;
}

// Init the task scheduler 
int task_scheduler_init(ngscope_task_scheduler_t* task_scheduler,
                            prog_args_t prog_args){
                            //srsran_rf_t* rf, 
                            //srsran_cell_t* cell, 
                            //srsran_ue_sync_t* ue_sync){
    float search_cell_cfo = 0;
    task_scheduler->prog_args = prog_args;

    cell_search_cfg_t cell_detect_config = {.max_frames_pbch      = SRSRAN_DEFAULT_MAX_FRAMES_PBCH,
                                        .max_frames_pss       = SRSRAN_DEFAULT_MAX_FRAMES_PSS,
                                        .nof_valid_pss_frames = SRSRAN_DEFAULT_NOF_VALID_PSS_FRAMES,
                                        .init_agc             = 0,
                                        .force_tdd            = false};
   
    // Copy the prameters 
    task_scheduler->prog_args = prog_args;
 
    // First of all, start the radio and get the cell information
    radio_init_and_start(&task_scheduler->rf, &task_scheduler->cell, prog_args, 
                                                &cell_detect_config, &search_cell_cfo);
           
    // Copy the cell info to the  
    printf("before copying to cell \n");
    pthread_mutex_lock(&cell_mutex); 
    memcpy(&cell_vec[prog_args.rf_index], &(task_scheduler->cell), sizeof(srsran_cell_t));
    printf("\n\nFinished copying to cell:%d prb:%d \n", prog_args.rf_index, cell_vec[prog_args.rf_index].nof_prb);
    pthread_mutex_unlock(&cell_mutex); 

    // Next, let's get the ue_sync ready
    ue_sync_init_imp(&task_scheduler->ue_sync, &task_scheduler->rf, &task_scheduler->cell, 
                                          &cell_detect_config, prog_args, search_cell_cfo); 

    printf("pthread lock ack \n");
    pthread_mutex_lock(&ack_mutex); 
    init_pending_ack(&ack_list);
    pthread_mutex_unlock(&ack_mutex); 

    printf("pthread unlock ack \n");

    return SRSRAN_SUCCESS;
}

void copy_sf_sync_buffer(cf_t* source[SRSRAN_MAX_PORTS],
                         cf_t* dest[SRSRAN_MAX_PORTS],
                         uint32_t max_num_samples)
{
    for(int p=0; p<SRSRAN_MAX_PORTS; p++){
        memcpy(dest[p], source[p], max_num_samples*sizeof(cf_t));
    }
    return;
}
void assign_task_to_decoder(ngscope_task_scheduler_t* task_scheduler,
                                int idle_idx, uint32_t sf_idx, uint32_t sfn,
                                uint32_t max_num_samples,
                                cf_t* IQ_buffer[SRSRAN_MAX_PORTS],
                                FILE* file)
{

    //--> Lock sf buffer
    pthread_mutex_lock(&sf_buffer[idle_idx].sf_mutex);

    // Tell the scheduler that we now busy
    pthread_mutex_lock(&token_mutex);
    sf_token[idle_idx] = true;
    pthread_mutex_unlock(&token_mutex);

    
    //printf("TTI:%d --> sfn:%d sf_idx:%d\n", sf_idx + sfn * 10, sfn, sf_idx);


            
    sf_buffer[idle_idx].sf_idx  = sf_idx;
    sf_buffer[idle_idx].sfn     = sfn;

    fprintf(file, "sfn_idx, sfn\n");
    fprintf(file, "%d, %d\n", sf_idx, sfn);
    // copy the buffer source:sync_buffer dest: IQ_buffer
    //copy_sf_sync_buffer(sync_buffer, sf_buffer[idle_idx].IQ_buffer, max_num_samples);
    for(int p=0; p<task_scheduler->prog_args.rf_nof_rx_ant; p++){
        memcpy(sf_buffer[idle_idx].IQ_buffer[p], IQ_buffer[p], max_num_samples*sizeof(cf_t));
    }

    // Tell the corresponding idle thread to process the signal
    //printf("Send the conditional signal to the %d-th decoder!\n", idle_idx);
    pthread_cond_signal(&sf_buffer[idle_idx].sf_cond);

    //--> Unlock sf buffer
    pthread_mutex_unlock(&sf_buffer[idle_idx].sf_mutex);

    return;
}

// This is used to start decoding thread
int task_scheduler_start(ngscope_task_scheduler_t* task_scheduler){
                            //prog_args_t prog_args){ 
                            //srsran_cell_t* cell, 
                            //srsran_ue_sync_t* ue_sync){
    int ret;
    int nof_decoder = task_scheduler->prog_args.nof_decoder;
    uint32_t pkt_errors = 0, pkt_total = 0;
    //int rf_idx      = task_scheduler->prog_args.rf_index;

    // contains nof_ul_dci, nof_dl_dci, and ul_msg / dl_msg for dci
    ngscope_dci_per_sub_t       dci_per_sub; // empty place hoder for skipped frames 
    // contains tti, cell_iondex, dci_per_sub, csi_amp
    ngscope_status_buffer_t     dci_ret;

    memset(&dci_per_sub, 0, sizeof(ngscope_dci_per_sub_t));
    memset(&dci_ret, 0, sizeof(ngscope_status_buffer_t));

    uint32_t max_num_samples = 3 * SRSRAN_SF_LEN_PRB(task_scheduler->cell.nof_prb); /// Length in complex samples
    printf("nof_prb:%d \n max_sample:%d\n", task_scheduler->cell.nof_prb, max_num_samples);

    /************** Setting up the UE sync buffer ******************/
    cf_t* sync_buffer[SRSRAN_MAX_PORTS];
    cf_t* buffers[SRSRAN_MAX_CHANNELS] = {};
    for (int j = 0; j < task_scheduler->prog_args.rf_nof_rx_ant; j++) {
        sync_buffer[j] = srsran_vec_cf_malloc(max_num_samples);
    }
    // Set the buffer for ue_sync
    for (int p = 0; p < SRSRAN_MAX_PORTS; p++) {
        buffers[p] = sync_buffer[p];
    }
    /************** END OF setting up the UE sync buffer ******************/

    // init the subframe buffer
    // srsran_ue_dl_t     ue_dl;
    // srsran_ue_dl_cfg_t ue_dl_cfg;
    // srsran_dl_sf_cfg_t dl_sf;
    // srsran_pdsch_cfg_t pdsch_cfg;
    // srsran_cell_t      cell;
    // prog_args_t        prog_args;
    // int                decoder_idx;
    ngscope_dci_decoder_t   dci_decoder[MAX_NOF_DCI_DECODER];
    // max number of code block (size) to allocate
    srsran_softbuffer_rx_t  rx_softbuffers[SRSRAN_MAX_CODEWORDS];
    pthread_t               dci_thd[MAX_NOF_DCI_DECODER];

    // Init the UE MIB decoder
    srsran_ue_mib_t         ue_mib;    
    mib_init_imp(&ue_mib, sync_buffer, &task_scheduler->cell);

    /********************** Set up the tmp buffer **********************/
    task_tmp_buffer_t   task_tmp_buffer;
    task_tmp_buffer.header   = 0;
    task_tmp_buffer.tail     = 0;
    //task_tmp_buffer.nof_buf  = 0;

    printf("rf_nof_rx_ant: %d \n", task_scheduler->prog_args.rf_nof_rx_ant);
    // init the buffer
    for(int i=0; i<MAX_TMP_BUFFER; i++){
        for (int j = 0; j < task_scheduler->prog_args.rf_nof_rx_ant; j++) {
            task_tmp_buffer.sf_buf[i].IQ_buffer[j] = srsran_vec_cf_malloc(max_num_samples);
        }
    }
    
    /********** End of setting up the tmp buffer **********************/
    printf("number of decoders: %d \n", nof_decoder);
    for(int i=0;i<nof_decoder;i++){
        // init the subframe buffer
        for (int j = 0; j < task_scheduler->prog_args.rf_nof_rx_ant; j++) {
            sf_buffer[i].IQ_buffer[j] = srsran_vec_cf_malloc(max_num_samples);
        }  
        //init the decoder (in order to decode the real data)
        printf("init the decoder!\n");
        dci_decoder_init(&dci_decoder[i], task_scheduler->prog_args, &task_scheduler->cell, 
                                        sf_buffer[i].IQ_buffer, rx_softbuffers, i);

        //mib_init_imp(&ue_mib[i], sf_buffer[i].IQ_buffer, &task_scheduler->cell);
        printf("start the decoding thread!\n");
        // this dci_decoder_thread is the hardest part
        pthread_create( &dci_thd[i], NULL, dci_decoder_thread, (void*)&dci_decoder[i]);
    }
    
    // Let's sleep for 5 seconds and wait for the decoder to be ready!
    // the following part is being run in pararell with dci_decoder_thread
    sleep(1);
    //srsran_ue_mib_t ue_mib;    
    //mib_init_imp(&ue_mib, sf_buffer[i].sf_buffer, cell);

    int         sf_cnt = 0; 
    uint32_t    sfn = 0;
    bool        decode_pdcch = false;
    FILE* file;
    char sfn_log[128];
    system("mkdir ./sfn_log");
    sprintf(sfn_log, "./sfn_log/sfn_log.csv");
    file = fopen(sfn_log, "w+");

    while(!go_exit && (sf_cnt < task_scheduler->prog_args.nof_subframes || task_scheduler->prog_args.nof_subframes == -1)) {
        /*  Get the subframe data and put it into the buffer */
        ret = srsran_ue_sync_zerocopy(&task_scheduler->ue_sync, buffers, max_num_samples);
        //printf("RET is:%d\n", ret); 
        if (ret < 0) {
            ERROR("Error calling srsran_ue_sync_work()");
        }else if(ret == 1){
            uint32_t sf_idx = srsran_ue_sync_get_sfidx(&task_scheduler->ue_sync);
            // printf("Get %d-th subframe \n", sf_idx);
            sf_cnt ++; 
            /********************* SFN handling *********************/
           
            if ( (sf_idx == 0) || (decode_pdcch == false) ) {
                // update SFN when sf_idx is 0 
                uint32_t sfn_tmp = 0;
                ue_mib_decode_sfn(&ue_mib, &task_scheduler->cell, &sfn_tmp, decode_pdcch);

                // printf("Current sfn:%d ",sfn);
                if(sfn != sfn_tmp){
                    printf("current sfn:%d decoded sfn:%d\n",sfn, sfn_tmp);
                }
                if(sfn_tmp > 0){
                    //printf("decoded sfn from:%d\n",sfn_tmp);
                    sfn = sfn_tmp;
                    decode_pdcch = true;
                }
                //printf("\n");
            }
            /******************* END OF SFN handling *******************/
           
            /***************** Tell the decoder to decode the PDCCH *********/          
            if(decode_pdcch){  // We only decode when we got the SFN

                /** Now we need to know where shall we put the IQ data of each subframe*/
                int idle_idx  = -1;

                idle_idx  =  find_idle_decoder(nof_decoder);
                if(idle_idx < 0){
                    //printf("Skiping %d subframe since Decoder Blocked! \
                            We suggest increasing the number deocder per cell.\n", sfn*10 + sf_idx);

                    /* Store the data into a tmp buffer. Later, when we have idle decoder, we will decode it*/ 
                    task_tmp_buffer.header++;
                    task_tmp_buffer.header = task_tmp_buffer.header % MAX_TMP_BUFFER; // advace the header first
                    //printf("Nof buffer:%d %d\n", task_tmp_buffer.header, task_tmp_buffer.nof_buf);

                    task_tmp_buffer.sf_buf[task_tmp_buffer.header].sf_idx   = sf_idx;
                    task_tmp_buffer.sf_buf[task_tmp_buffer.header].sfn      = sfn;
                    for(int p=0; p<task_scheduler->prog_args.rf_nof_rx_ant; p++){
                        memcpy(task_tmp_buffer.sf_buf[task_tmp_buffer.header].IQ_buffer[p], 
                                                sync_buffer[p], max_num_samples*sizeof(cf_t));
                    }   
                printf("PDSCH-BLER: %5.2f%%", (float)100 * pkt_errors / pkt_total);

                    
                    if((sf_idx == 9)) {
                        sfn++;  // we increase the sfn incase MIB decoding failed
                        if(sfn == 1024){
                            sfn = 0;
                            pkt_errors = 0;
                            pkt_total = 0; 
                        }
                    }
                    continue;
                }

                // Assign the Task to the corresponding decoder 
                assign_task_to_decoder(task_scheduler, idle_idx, sf_idx, sfn, max_num_samples, sync_buffer, file);

                /* Getting here means we have idle decoder, so we check if we have sf in tmp buffer*/ 
                if(task_tmp_buffer.header !=  task_tmp_buffer.tail){
                    //printf("We have something in the tmp buffer!\n"); 
                    while(true){
                        idle_idx  =  find_idle_decoder(nof_decoder);
                        if(idle_idx < 0){ 
                            break;  
                        }else{
                            // Assign the Task to the corresponding decoder
                            task_tmp_buffer.tail++; 
                            task_tmp_buffer.tail = task_tmp_buffer.tail % MAX_TMP_BUFFER;
                            int tmp_buf_idx = task_tmp_buffer.tail;
                            int tmp_sf_idx  = task_tmp_buffer.sf_buf[tmp_buf_idx].sf_idx;
                            int tmp_sfn     = task_tmp_buffer.sf_buf[tmp_buf_idx].sfn;

                            //printf("Assigning tti:%d to the %d-th decoder since it is idle!\n", 
                            //                                tmp_sfn * 10 + tmp_sf_idx, idle_idx); 

                            assign_task_to_decoder(task_scheduler, idle_idx, tmp_sf_idx, tmp_sfn, max_num_samples,
                                     task_tmp_buffer.sf_buf[tmp_buf_idx].IQ_buffer, file);
                            if(task_tmp_buffer.header == task_tmp_buffer.tail){
                                break;
                            }
                        }
                    }
                }
            }
            if((sf_idx == 9)) {
                sfn++;  // we increase the sfn in case MIB decoding failed
                if(sfn == 1024){ sfn = 0; }
            }
        }// endof if(decode_pdcch)
    }// end of while


//--> Deal with the exit free memory 
    /* Wait for the decoder thread to finish*/
    for(int i=0;i<nof_decoder;i++){
        // Tell the decoder thread to exit in case 
        // they are still waiting for the signal

        pthread_cond_signal(&sf_buffer[i].sf_cond);
        pthread_join(dci_thd[i], NULL);
    }
    for(int i=0;i<nof_decoder;i++){
        srsran_ue_dl_free(&dci_decoder[i].ue_dl);
        //free the buffer
        for(int j=0;  j < task_scheduler->prog_args.rf_nof_rx_ant; j++){
            free(sf_buffer[i].IQ_buffer[j]);
        }
    } 
        
    srsran_ue_mib_free(&ue_mib);
    // free the ue_sync
    srsran_ue_sync_free(&task_scheduler->ue_sync);
    for(int j=0; j<task_scheduler->prog_args.rf_nof_rx_ant; j++){
        free(sync_buffer[j]);
    }
    radio_stop(&task_scheduler->rf);
    return SRSRAN_SUCCESS;
}

void* task_scheduler_thread(void* p){
    // all the initizialized program arguements are passed from ngscope_main.c file
    prog_args_t* prog_args = (prog_args_t*)p;

    ngscope_task_scheduler_t task_scheduler;

    task_scheduler_init(&task_scheduler, *prog_args);
    task_scheduler_start(&task_scheduler);

    printf("Close %d-th RF devices!\n", prog_args->rf_index);
    return NULL;
}



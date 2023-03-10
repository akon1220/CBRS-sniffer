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
#include "ngscope/hdr/dciLib/phich_decoder.h"
#include "ngscope/hdr/dciLib/time_stamp.h"


extern bool                 go_exit;
extern ngscope_sf_buffer_t  sf_buffer[MAX_NOF_DCI_DECODER];
extern bool                 sf_token[MAX_NOF_DCI_DECODER];
extern pthread_mutex_t      token_mutex; 

extern dci_ready_t         dci_ready;
extern ngscope_status_buffer_t    dci_buffer[MAX_DCI_BUFFER];

// For decoding phich
extern pend_ack_list       ack_list;
extern pthread_mutex_t     ack_mutex;

int dci_decoder_init(ngscope_dci_decoder_t*     dci_decoder,
                        prog_args_t             prog_args,
                        srsran_cell_t*          cell,
                        cf_t*                   sf_buffer[SRSRAN_MAX_PORTS],
                        srsran_softbuffer_rx_t* rx_softbuffers,
                        int                     decoder_idx){
    // Init the args
    dci_decoder->prog_args  = prog_args;
    dci_decoder->cell       = *cell;

    if (srsran_ue_dl_init(&dci_decoder->ue_dl, sf_buffer, cell->nof_prb, prog_args.rf_nof_rx_ant)) {
        ERROR("Error initiating UE downlink processing module");
        exit(-1);
    }
    if (srsran_ue_dl_set_cell(&dci_decoder->ue_dl, *cell)) {
        ERROR("Error initiating UE downlink processing module");
        exit(-1);
    }

    ZERO_OBJECT(dci_decoder->ue_dl_cfg);
    ZERO_OBJECT(dci_decoder->dl_sf);
    ZERO_OBJECT(dci_decoder->pdsch_cfg);

    /************************* Init dl_sf **************************/
    if (cell->frame_type == SRSRAN_TDD && prog_args.tdd_special_sf >= 0 && prog_args.sf_config >= 0) {
        dci_decoder->dl_sf.tdd_config.ss_config  = prog_args.tdd_special_sf;
        //dci_decoder->dl_sf.tdd_config.sf_config  = prog_args.sf_config;
        dci_decoder->dl_sf.tdd_config.sf_config  = 2; 
        dci_decoder->dl_sf.tdd_config.configured = true;
    }
    dci_decoder->dl_sf.tdd_config.ss_config  = prog_args.tdd_special_sf;
    //dci_decoder->dl_sf.tdd_config.sf_config  = prog_args.sf_config;
    dci_decoder->dl_sf.tdd_config.sf_config  = 2; 
    dci_decoder->dl_sf.tdd_config.configured = true;

    /************************* Init ue_dl_cfg **************************/
    srsran_chest_dl_cfg_t chest_pdsch_cfg = {};
    chest_pdsch_cfg.cfo_estimate_enable   = prog_args.enable_cfo_ref;
    chest_pdsch_cfg.cfo_estimate_sf_mask  = 1023;
    chest_pdsch_cfg.estimator_alg         = srsran_chest_dl_str2estimator_alg(prog_args.estimator_alg);
    chest_pdsch_cfg.sync_error_enable     = true;

    // Set PDSCH channel estimation (we don't consider MBSFN)
    dci_decoder->ue_dl_cfg.chest_cfg = chest_pdsch_cfg;

    /************************* Init pdsch_cfg **************************/
    dci_decoder->pdsch_cfg.meas_evm_en = true;
    // Allocate softbuffer buffers
    //srsran_softbuffer_rx_t rx_softbuffers[SRSRAN_MAX_CODEWORDS];
    for (uint32_t i = 0; i < SRSRAN_MAX_CODEWORDS; i++) {
        dci_decoder->pdsch_cfg.softbuffers.rx[i] = &rx_softbuffers[i];
        srsran_softbuffer_rx_init(dci_decoder->pdsch_cfg.softbuffers.rx[i], cell->nof_prb);
    }

    dci_decoder->pdsch_cfg.rnti = prog_args.rnti;
    dci_decoder->decoder_idx    = decoder_idx;
    return SRSRAN_SUCCESS;
}

int dci_decoder_decode(ngscope_dci_decoder_t*       dci_decoder,
                            uint32_t                sf_idx,
                            uint32_t                sfn,
                            ngscope_dci_msg_t       dci_array[][MAX_CANDIDATES_ALL],
                            srsran_dci_location_t   dci_location[MAX_CANDIDATES_ALL],
                            ngscope_dci_per_sub_t*  dci_per_sub,
                            FILE* file
                            )
{
    uint32_t tti = sfn * 10 + sf_idx;

    bool decode_pdsch = false;
      
    // Shall we decode the PDSCH of the current subframe?
    if (dci_decoder->prog_args.rnti != SRSRAN_SIRNTI) {
        decode_pdsch = true;
        if (srsran_sfidx_tdd_type(dci_decoder->dl_sf.tdd_config, sf_idx) == SRSRAN_TDD_SF_U) {
            decode_pdsch = false;
        }
    } else {
        /* We are looking for SIB1 Blocks, search only in appropiate places */
        if ((sf_idx == 5 && (sfn % 2) == 0)) {
            decode_pdsch = true;
        } else {
            decode_pdsch = false;
        }
    }

    //if(sf_idx % 1 == 0)    
    //    decode_pdsch = true;

    decode_pdsch = true;
    printf("cell frame type: %d", dci_decoder->cell.frame_type);

    if ( (dci_decoder->cell.frame_type == SRSRAN_TDD) && 
        (srsran_sfidx_tdd_type(dci_decoder->dl_sf.tdd_config, sf_idx) == SRSRAN_TDD_SF_U) ){
        printf("TDD uplink subframe skip\n");
        decode_pdsch = false;
    }
 
    int n = 0;
    // Now decode the PDSCH
    if(decode_pdsch){
        uint32_t tm = 3;
        dci_decoder->dl_sf.tti                             = tti;
        dci_decoder->dl_sf.sf_type                         = SRSRAN_SF_NORM; //Ingore the MBSFN
        dci_decoder->ue_dl_cfg.cfg.tm                      = (srsran_tm_t)tm;
        dci_decoder->ue_dl_cfg.cfg.pdsch.use_tbs_index_alt = true;

        //n = srsran_ngscope_search_all_space_yx(&dci_decoder->ue_dl, &dci_decoder->dl_sf, 
        //                                    &dci_decoder->ue_dl_cfg, &dci_decoder->pdsch_cfg);

        uint64_t t1 = timestamp_us();       
        // get device ID (C RNTI) and stuff 
        n = srsran_ngscope_search_all_space_array_yx(&dci_decoder->ue_dl, &dci_decoder->dl_sf, &dci_decoder->ue_dl_cfg, 
                                            &dci_decoder->pdsch_cfg, dci_array, dci_location, dci_per_sub);
        uint64_t t2 = timestamp_us();        
        printf("time_spend:%ld (us)\n", t2-t1);
        printf("decoder:%d finish decoding. time_spend:%ld (us)\n", dci_decoder->decoder_idx, t2-t1);
        fprintf(file, "%d, %ld\n", dci_decoder->decoder_idx, t2-t1);


    } 
    return SRSRAN_SUCCESS;
}
int get_target_dci(ngscope_dci_msg_t* msg, int nof_msg, uint16_t targetRNTI){
    for(int i=0; i<nof_msg; i++){
        if(msg[i].rnti == targetRNTI){
            return i;
        }
    }
    return -1;
}
int dci_decoder_phich_decode(ngscope_dci_decoder_t*       dci_decoder,
                                  uint32_t                tti,
                                  ngscope_dci_per_sub_t*  dci_per_sub,
                                  FILE* fp)
{
    float rsrp0=0.0, rsrp1=0.0, rsrq=0.0, enodebrate=0.0, uerate=0.0;
    bool acks[SRSRAN_MAX_CODEWORDS] = {false};

    uint16_t targetRNTI = dci_decoder->prog_args.rnti;
    // printf("target RNTI: %d", targetRNTI);
    if(targetRNTI > 0){
        if(dci_per_sub->nof_ul_dci > 0){
            int idx = get_target_dci(dci_per_sub->ul_msg, dci_per_sub->nof_ul_dci, targetRNTI);
            if(idx >= 0){
                uint32_t n_dmrs         = dci_per_sub->ul_msg[idx].phich.n_dmrs;
                uint32_t n_prb_tilde    = dci_per_sub->ul_msg[idx].phich.n_prb_tilde;
                pthread_mutex_lock(&ack_mutex);
                phich_set_pending_ack(&ack_list, TTI_RX_ACK(tti), n_prb_tilde, n_dmrs);
                pthread_mutex_unlock(&ack_mutex);
            }
        }
        srsran_phich_res_t phich_res;
        bool ack_available = false;
        ack_available = decode_phich(&dci_decoder->ue_dl, &dci_decoder->dl_sf, &dci_decoder->ue_dl_cfg, &ack_list, &phich_res);
        // after decoding phich, you can log information about RSRP, RSRQ, SNR, etc
        // usually not available
        if(ack_available){
            printf("Get PHICH: ack%d distance:%f\n", phich_res.ack_value, phich_res.distance);
            fprintf(fp, "%d\t%f\t", phich_res.ack_value, phich_res.distance);
        }else{
            fprintf(fp, "%d\t%f\t", -1, -1.0);
        }
        printf("ACK: %d \n", phich_res.ack_value);

        uint32_t enb_bits = ((dci_decoder->pdsch_cfg.grant.tb[0].enabled ? dci_decoder->pdsch_cfg.grant.tb[0].tbs : 0) +
                                 (dci_decoder->pdsch_cfg.grant.tb[1].enabled ? dci_decoder->pdsch_cfg.grant.tb[1].tbs : 0));

        uint32_t ue_bits = ((acks[0] ? dci_decoder->pdsch_cfg.grant.tb[0].tbs : 0) + (acks[1] ? dci_decoder->pdsch_cfg.grant.tb[1].tbs : 0));
        // SNR
        fprintf(fp, "%f\t%f\t%f\t%f\t%f\t%f\n", dci_decoder->ue_dl.chest_res.snr_db,
        SRSRAN_VEC_EMA(dci_decoder->ue_dl.chest_res.rsrp_dbm, rsrq, 0.1f), SRSRAN_VEC_EMA(dci_decoder->ue_dl.chest_res.rsrp_port_dbm[0], rsrp0, 0.05f), 
        SRSRAN_VEC_EMA(dci_decoder->ue_dl.chest_res.rsrp_port_dbm[1], rsrp1, 0.05f), SRSRAN_VEC_EMA(enb_bits / 1000.0f, enodebrate, 0.05f), 
        SRSRAN_VEC_EMA(ue_bits / 1000.0f, uerate, 0.001f));
        //
    }
    return 0;
}


void empty_dci_array(ngscope_dci_msg_t   dci_array[][MAX_CANDIDATES_ALL],
                        srsran_dci_location_t   dci_location[MAX_CANDIDATES_ALL],
                        ngscope_dci_per_sub_t*  dci_per_sub){
    for(int i=0; i<MAX_NOF_FORMAT+1; i++){
        for(int j=0; j<MAX_CANDIDATES_ALL; j++){
            ZERO_OBJECT(dci_location[j]);
            ZERO_OBJECT(dci_array[i][j]);
        }
    }
    
    dci_per_sub->nof_dl_dci = 0;
    dci_per_sub->nof_ul_dci = 0;
    for(int i=0; i<MAX_DCI_PER_SUB; i++){
        ZERO_OBJECT(dci_per_sub->dl_msg[i]); 
        ZERO_OBJECT(dci_per_sub->ul_msg[i]); 
    }
    return;
}

void* dci_decoder_thread(void* p){
    ngscope_dci_decoder_t*  dci_decoder = (ngscope_dci_decoder_t*)p;
    ngscope_dci_msg_t       dci_array[MAX_NOF_FORMAT+1][MAX_CANDIDATES_ALL];
    srsran_dci_location_t   dci_location[MAX_CANDIDATES_ALL];
    ngscope_dci_per_sub_t   dci_per_sub; 
    ngscope_status_buffer_t        dci_ret;
    FILE *fp2 = fopen("./decoding_time.csv", "w+");
    fprintf(fp2, "decoder_idx, decode_time\n");

    FILE *fp = fopen("./phich.txt","w+");

    int rf_idx     = dci_decoder->prog_args.rf_index;

    printf("Decoder thread idx:%d\n\n\n", dci_decoder->decoder_idx);
    while(!go_exit){
        empty_dci_array(dci_array, dci_location, &dci_per_sub);
        //printf("dci decoder -1\n");
        printf("lock the buffer");
//--->  Lock the buffer
        pthread_mutex_lock(&sf_buffer[dci_decoder->decoder_idx].sf_mutex);
    
        // We release the token in the last minute just before the waiting of the condition signal 
        pthread_mutex_lock(&token_mutex);
        if(sf_token[dci_decoder->decoder_idx] == true){
            sf_token[dci_decoder->decoder_idx] = false;
        }
        pthread_mutex_unlock(&token_mutex);

//--->  Wait the signal 
        //printf("%d-th decoder is waiting for conditional signal!\n", dci_decoder->decoder_idx);

        pthread_cond_wait(&sf_buffer[dci_decoder->decoder_idx].sf_cond, 
                          &sf_buffer[dci_decoder->decoder_idx].sf_mutex);
        //printf("%d-th decoder Get the conditional signal!\n", dci_decoder->decoder_idx);
    
        uint32_t sfn    = sf_buffer[dci_decoder->decoder_idx].sfn;
        uint32_t sf_idx = sf_buffer[dci_decoder->decoder_idx].sf_idx;
        uint32_t tti    = sfn * 10 + sf_idx;

        //printf("decoder:%d Get the signal! sfn:%d sf_idx:%d tti:%d\n", \
                            dci_decoder->decoder_idx, sfn, sf_idx, sfn * 10 + sf_idx);

        //dci_decoder_decode(dci_decoder, sf_idx, sfn);

        // Now decode the PDSCH
        printf("decoding PDSCH \n");
        // get C RNTI
        dci_decoder_decode(dci_decoder, sf_idx, sfn, dci_array, dci_location, &dci_per_sub, fp2);
//--->  Unlock the buffer
        //usleep(2000);
        pthread_mutex_unlock(&sf_buffer[dci_decoder->decoder_idx].sf_mutex);
        //printf("End of decoding decoder_idx:%d sfn:%d sf_idx:%d tti:%d\n", 
        //                    dci_decoder->decoder_idx, sfn, sf_idx, sfn * 10 + sf_idx);

        printf("decoding phich \n");
        dci_decoder_phich_decode(dci_decoder, tti, &dci_per_sub, fp);
        dci_ret.dci_per_sub  = dci_per_sub;
        dci_ret.tti          = sfn *10 + sf_idx;
        dci_ret.cell_idx     = rf_idx;


        // dci_ret.rsrp = srsran_convert_dB_to_power(dci_decoder->ue_dl.chest_res.rsrp);
        // printf("rsrp is %f \n", dci_ret.rsrp);   

        // for each subcarrier (1200 in total)
        for(int i=0; i< 12 * dci_decoder->cell.nof_prb; i++){
            //xuliang change to port2
            dci_ret.csi_amp[i] = srsran_convert_amplitude_to_dB(cabsf(dci_decoder->ue_dl.chest_res.ce[0][0][i])); 
        }
       // // put the dci into the dci buffer
        pthread_mutex_lock(&dci_ready.mutex);
        dci_buffer[dci_ready.header] = dci_ret;
        dci_ready.header = (dci_ready.header + 1) % MAX_DCI_BUFFER;
        if(dci_ready.nof_dci < MAX_DCI_BUFFER){
            dci_ready.nof_dci++;
        }
        printf("TTI :%d ul_dci: %d dl_dci:%d nof_dci:%d\n", dci_ret.tti, dci_per_sub.nof_ul_dci, 
                                                dci_per_sub.nof_dl_dci, dci_ready.nof_dci);
        pthread_cond_signal(&dci_ready.cond);
        pthread_mutex_unlock(&dci_ready.mutex);
    }
    //srsran_ue_dl_free(&dci_decoder->ue_dl);
    printf("Close %d-th DCI decoder!\n",dci_decoder->decoder_idx);
    return NULL;
}

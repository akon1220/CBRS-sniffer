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

/* We force to use srsgui */
#include "srsgui/srsgui.h"
#include "ngscope/hdr/dciLib/status_plot.h"

// time (x axis)
#define NOF_PLOT_SF 500
#define MOV_AVE_LEN 5 
extern bool go_exit;
extern pthread_mutex_t     plot_mutex;
extern ngscope_plot_t      plot_data;
extern pthread_cond_t      plot_cond;
// PLOT_SF is 10 (because of 10 subframes)

uint16_t contineous_handled_sf(ngscope_plot_cell_t* q){
    uint16_t start  = q->header;
    uint16_t end    = q->dci_touched;
    uint16_t untouched_idx;

    printf("start header: %d \n", start);
    printf("end header: %d \n", end);

    // init the untouched index
    untouched_idx = start;

    // No change return
    if( end == start)
        return start;

    // unwrapping
    if( end < start){ end += PLOT_SF;}

    for(int i=start+1;i<=end;i++){
        uint16_t index = i % PLOT_SF;
        untouched_idx = i;
        if(q->token[index] == 0){
            // return when we encounter a token that has been taken
            untouched_idx -= 1;
            return  untouched_idx % PLOT_SF;
        }
    }
    return  untouched_idx % PLOT_SF;
}

int movingAvg(int *ptrArrNumbers, int *ptrSum, int pos, int len, int nextNum)
{
  //Subtract the oldest number from the prev sum, add the new number
  *ptrSum = *ptrSum - ptrArrNumbers[pos] + nextNum;
  //Assign the nextNum to the position in the array
  ptrArrNumbers[pos] = nextNum;
  //return the average
  return *ptrSum / len;
}


void left_shift_vec(float* vec){
    for(int i=0; i< NOF_PLOT_SF-1; i++){
        vec[i] = vec[i+1];
    }
    return;
}
void* plot_thread_run(void* arg)
{
    int nof_prb;
    int nof_subcarrier;

    nof_prb = plot_data.cell_prb[0];
    //nof_sub is 1200
    nof_subcarrier = 12 * nof_prb;
 
    sdrgui_init();
    //plot_real_t csi;
    plot_real_t p_prb_ul, p_prb_dl;
//    plot_scatter_t pdsch, pdcch;
    plot_waterfall_t csi_water;
//    plot_real_t rsrp;

    plot_waterfall_init(&csi_water, nof_subcarrier, 100); // xaxis and y axis
    plot_waterfall_setTitle(&csi_water, "Channel Response - Magnitude");
    plot_complex_setPlotXLabel(&csi_water, "Subcarrier Index"); 
    plot_complex_setPlotYLabel(&csi_water, "dB"); 
    plot_waterfall_setSpectrogramXLabel(&csi_water, "Subcarrier Index"); 
    plot_waterfall_setSpectrogramYLabel(&csi_water, "Time"); 

//    plot_real_init(&rsrp);
//    plot_real_setTitle(&rsrp, "RSRP dBm");
//    plot_real_setLabels(&rsrp, "Time", "PRB");
//    plot_real_setYAxisScale(&rsrp, 0, nof_prb);
//    plot_real_addToWindowGrid(&rsrp, (char*)"pdsch_ue", 0, 0);

//    plot_scatter_init(&pdsch);
//    plot_scatter_setTitle(&pdsch, "PDSCH - Equalized Symbols");
//    plot_scatter_setXAxisScale(&pdsch, -4, 4);
//    plot_scatter_setYAxisScale(&pdsch, -4, 4);
//    plot_scatter_addToWindowGrid(&pdsch, (char*)"pdsch_ue", 0, 0);
//        
//    plot_scatter_init(&pdcch);
//    plot_scatter_setTitle(&pdcch, "PDCCH - Equalized Symbols");
//    plot_scatter_setXAxisScale(&pdcch, -4, 4);
//    plot_scatter_setYAxisScale(&pdcch, -4, 4);
//    plot_real_addToWindowGrid(&pdcch, (char*)"pdsch_ue", 0, 1);

//    plot_real_init(&csi);
//    plot_real_setTitle(&csi, "Channel Response - Magnitude");
//    plot_real_setLabels(&csi, "Subcarrier Index", "dB");
//    plot_real_setYAxisScale(&csi, -40, 40);
//    plot_real_addToWindowGrid(&csi, (char*)"pdsch_ue", 0, 1);

    plot_real_init(&p_prb_dl);
    plot_real_setTitle(&p_prb_dl, "Downlink PRB usage");
    plot_real_setLabels(&p_prb_dl, "Time", "PRB");
    plot_real_setYAxisScale(&p_prb_dl, 0, nof_prb);
    plot_real_addToWindowGrid(&p_prb_dl, (char*)"pdsch_ue", 0, 0);

    plot_real_init(&p_prb_ul);
    plot_real_setTitle(&p_prb_ul, "Uplink PRB usage");
    plot_real_setLabels(&p_prb_ul, "Time", "PRB");
    plot_real_setYAxisScale(&p_prb_ul, 0, nof_prb);
    plot_real_addToWindowGrid(&p_prb_ul, (char*)"pdsch_ue", 1, 0);

    int prb_idx = 0;
    float cell_dl_prb[NOF_PLOT_SF] = {0};
    float cell_ul_prb[NOF_PLOT_SF] = {0};
    float cell_dl_prb_ave[NOF_PLOT_SF] = {0};
    float cell_ul_prb_ave[NOF_PLOT_SF] = {0};
 
    int mv_ave_buf_dl[MOV_AVE_LEN] = {0};
    int mv_ave_buf_ul[MOV_AVE_LEN] = {0};

    int sum_dl = 0;
    int sum_ul = 0;

    while(!go_exit){
        pthread_mutex_lock(&plot_mutex);    
        pthread_cond_wait(&plot_cond, &plot_mutex); 

        int ave_idx = 0;
        /* Plot the data */
        uint16_t new_header     = contineous_handled_sf(&plot_data.plot_data_cell[0]);
        uint16_t last_header    = plot_data.plot_data_cell[0].header;


        if( new_header != last_header){
            plot_data.plot_data_cell[0].header = new_header;
            if(new_header < last_header){ new_header += PLOT_SF;}
            //printf("now Update header: ->>>>> last header:%d new header:%d\n", last_header, new_header);
            // looping through each subframe (0 ~ 9)
            for(int i = last_header+1; i<= new_header; i++){
                // index is from 0 to 9 (10 in total)
                uint16_t index = i % PLOT_SF;
                printf("index: %d \n", index);
                float* csi_amp = plot_data.plot_data_cell[0].plot_data_sf[index].csi_amp;
                printf("csi_amp: %f", *csi_amp);
                //plot_real_setNewData(&csi, csi_amp, nof_sub); 
                plot_waterfall_appendNewData(&csi_water, csi_amp, nof_subcarrier);
//                float rsrp = plot_data.plot_data_cell[0].plot_data_sf[index].cfo;
                plot_data.plot_data_cell[0].token[index] = 0;
                uint32_t tti =  plot_data.plot_data_cell[0].plot_data_sf[index].tti;
                printf("plot_thread -> tti:%d\n", tti);
                int dl_prb = plot_data.plot_data_cell[0].plot_data_sf[index].cell_dl_prb;
                int ul_prb = plot_data.plot_data_cell[0].plot_data_sf[index].cell_ul_prb;
                printf("downlink PRB: %d \n", dl_prb);
                printf("uplink PRB: %d \n", ul_prb);
                // plot PRB usage
                if(prb_idx < NOF_PLOT_SF){
                    cell_dl_prb[prb_idx] = (float)dl_prb; 
                    cell_ul_prb[prb_idx] = (float)ul_prb; 
                    
                    cell_dl_prb_ave[prb_idx] = (float)movingAvg(mv_ave_buf_dl, &sum_dl, ave_idx, MOV_AVE_LEN, dl_prb); 
                    cell_ul_prb_ave[prb_idx] = (float)movingAvg(mv_ave_buf_ul, &sum_ul, ave_idx, MOV_AVE_LEN, ul_prb); 
                    ave_idx++;
                    if(ave_idx >= MOV_AVE_LEN){ ave_idx = 0;}

                    prb_idx++;
                }else{
                    left_shift_vec(cell_dl_prb);
                    left_shift_vec(cell_ul_prb);

                    left_shift_vec(cell_dl_prb_ave);
                    left_shift_vec(cell_ul_prb_ave);


                    cell_dl_prb[NOF_PLOT_SF-1] = (float)dl_prb; 
                    cell_ul_prb[NOF_PLOT_SF-1] = (float)ul_prb; 

                    // took average of 5
                    cell_dl_prb_ave[NOF_PLOT_SF-1] = (float)movingAvg(mv_ave_buf_dl, &sum_dl, ave_idx, MOV_AVE_LEN, dl_prb); 
                    cell_ul_prb_ave[NOF_PLOT_SF-1] = (float)movingAvg(mv_ave_buf_ul, &sum_ul, ave_idx, MOV_AVE_LEN, ul_prb); 

                    printf("cell_dl_prb: %f \nS", cell_dl_prb[NOF_PLOT_SF - 1]);
                    printf("cell_dl_prb_ave: %f \n", cell_dl_prb_ave[NOF_PLOT_SF - 1]);

                    ave_idx++;
                    if(ave_idx >= MOV_AVE_LEN){ ave_idx = 0;}
                }
                
                //plot_real_setNewData(&p_prb_dl, cell_dl_prb, NOF_PLOT_SF); 
                //plot_real_setNewData(&p_prb_ul, cell_ul_prb, NOF_PLOT_SF); 
 
                plot_real_setNewData(&p_prb_dl, cell_dl_prb_ave, NOF_PLOT_SF); 
                plot_real_setNewData(&p_prb_ul, cell_ul_prb_ave, NOF_PLOT_SF); 
            }
        } 
        
        pthread_mutex_unlock(&plot_mutex);    
    }
    return NULL;
}

void  plot_init_thread(pthread_t* plot_thread){
    pthread_attr_t     attr;
    struct sched_param param;
    param.sched_priority = 0;
    pthread_attr_init(&attr);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &param);
    if (pthread_create(plot_thread, NULL, plot_thread_run, NULL)) {
        perror("pthread_create");
        exit(-1);
    }
    return;
}

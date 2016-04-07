/* Copyright (C)
* 2015 - John Melton, G0ORX/N6LYT
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if_arp.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include "band.h"
#include "channel.h"
#include "discovered.h"
#include "mode.h"
#include "filter.h"
#include "old_protocol.h"
#include "radio.h"
#include "toolbar.h"

#define PI 3.1415926535897932F

#define DATA_PORT 1024

#define SYNC 0x7F
#define OZY_BUFFER_SIZE 512
#define OUTPUT_BUFFER_SIZE 1024

// ozy command and control
#define MOX_DISABLED    0x00
#define MOX_ENABLED     0x01

#define MIC_SOURCE_JANUS 0x00
#define MIC_SOURCE_PENELOPE 0x80
#define CONFIG_NONE     0x00
#define CONFIG_PENELOPE 0x20
#define CONFIG_MERCURY  0x40
#define CONFIG_BOTH     0x60
#define PENELOPE_122_88MHZ_SOURCE 0x00
#define MERCURY_122_88MHZ_SOURCE  0x10
#define ATLAS_10MHZ_SOURCE        0x00
#define PENELOPE_10MHZ_SOURCE     0x04
#define MERCURY_10MHZ_SOURCE      0x08
#define SPEED_48K                 0x00
#define SPEED_96K                 0x01
#define SPEED_192K                0x02
#define SPEED_384K                0x03
#define MODE_CLASS_E              0x01
#define MODE_OTHERS               0x00
#define ALEX_ATTENUATION_0DB      0x00
#define ALEX_ATTENUATION_10DB     0x01
#define ALEX_ATTENUATION_20DB     0x02
#define ALEX_ATTENUATION_30DB     0x03
#define LT2208_GAIN_OFF           0x00
#define LT2208_GAIN_ON            0x04
#define LT2208_DITHER_OFF         0x00
#define LT2208_DITHER_ON          0x08
#define LT2208_RANDOM_OFF         0x00
#define LT2208_RANDOM_ON          0x10

static int buffer_size=BUFFER_SIZE;

static int receiver;
static int display_width;

static int speed;

static int dsp_rate=48000;
static int output_rate=48000;

static int data_socket;
static struct sockaddr_in data_addr;
static int data_addr_length;

static int output_buffer_size;

static unsigned char control_in[5]={0x00,0x00,0x00,0x00,0x00};
static unsigned char control_out[5]={0x00,0x00,0x00,0x00,0x00};

static double tuning_phase;
static float phase=0.0f;

static int running;
static long ep4_sequence;

static int samples=0;

//static float left_input_buffer[BUFFER_SIZE];
//static float right_input_buffer[BUFFER_SIZE];
static double iqinputbuffer[BUFFER_SIZE*2];

//static float mic_left_buffer[BUFFER_SIZE];
//static float mic_right_buffer[BUFFER_SIZE];
static double micinputbuffer[BUFFER_SIZE*2];

//static float left_output_buffer[OUTPUT_BUFFER_SIZE];
//static float right_output_buffer[OUTPUT_BUFFER_SIZE];
static double audiooutputbuffer[BUFFER_SIZE*2];

//static float left_subrx_output_buffer[OUTPUT_BUFFER_SIZE];
//static float right_subrx_output_buffer[OUTPUT_BUFFER_SIZE];

//static float left_tx_buffer[OUTPUT_BUFFER_SIZE];
//static float right_tx_buffer[OUTPUT_BUFFER_SIZE];
static double micoutputbuffer[BUFFER_SIZE*2];

static short left_rx_sample;
static short right_rx_sample;
static short left_tx_sample;
static short right_tx_sample;

static unsigned char output_buffer[OZY_BUFFER_SIZE];
static int output_buffer_index=0;

static int command=0;

static pthread_t receive_thread_id;
static void start_receive_thread();
static void *receive_thread(void* arg);
static void process_ozy_input_buffer(char  *buffer);
static void process_bandscope_buffer(char  *buffer);
void ozy_send_buffer();

static unsigned char metis_buffer[1032];
static long send_sequence=-1;
static int metis_offset=8;

static int frequencyChanged=0;
static sem_t frequency_changed_sem;

static int metis_write(unsigned char ep,char* buffer,int length);
static void metis_start_stop(int command);
static void metis_send_buffer(char* buffer,int length);

void schedule_frequency_changed() {
//fprintf(stderr,"old_protocol: schedule_frequency_changed\n");
    //sem_wait(&frequency_changed_sem);
    frequencyChanged=1;
    //sem_post(&frequency_changed_sem);
}

static float sineWave(double* buf, int samples, float phase, float freq) {
    float phase_step = 2 * PI * freq / 192000.0F;
    int i;
    for (i = 0; i < samples; i++) {
        buf[i*2] = (double) sin(phase);
        phase += phase_step;
    }
    return phase;
}

static void setSpeed(int s) {
  int speed=SPEED_48K;
  output_buffer_size=OUTPUT_BUFFER_SIZE;
  switch(s) {
    case 48000:
        speed=SPEED_48K;
        output_buffer_size=OUTPUT_BUFFER_SIZE;
        break;
    case 96000:
        speed=SPEED_96K;
        output_buffer_size=OUTPUT_BUFFER_SIZE/2;
        break;
    case 192000:
        speed=SPEED_192K;
        output_buffer_size=OUTPUT_BUFFER_SIZE/4;
        break;
    case 384000:
        speed=SPEED_384K;
        output_buffer_size=OUTPUT_BUFFER_SIZE/8;
        break;
    default:
        fprintf(stderr,"Invalid sample rate: %d. Defaulting to 48K.\n",s);
        break;
  }

  //fprintf(stderr,"setSpeed sample_rate=%d speed=%d\n",s,speed);

  control_out[1]=control_out[1]&0xFC;
  control_out[1]=control_out[1]|speed;

}


void old_protocol_stop() {
  metis_start_stop(0);
  running=FALSE;
}

void old_protocol_init(int rx,int pixels) {
  int i;

  fprintf(stderr,"old_protocol_init\n");

  //int result=sem_init(&frequency_changed_sem, 0, 1);

  receiver=rx;
  display_width=pixels;

   // setup defaults
  control_out[0] = MOX_DISABLED;
  control_out[1] = CONFIG_BOTH
            | MERCURY_122_88MHZ_SOURCE
            | MERCURY_10MHZ_SOURCE
            | speed
            | MIC_SOURCE_PENELOPE;
  control_out[2] = MODE_OTHERS;
  control_out[3] = ALEX_ATTENUATION_0DB
            | LT2208_GAIN_OFF
            | LT2208_DITHER_ON
            | LT2208_RANDOM_ON;
  control_out[4] = 0;

  setSpeed(sample_rate);

  start_receive_thread();

  fprintf(stderr,"old_protocol_init: prime radio\n");
  for(i=8;i<OZY_BUFFER_SIZE;i++) {
    output_buffer[i]=0;
  }
  do {
    ozy_send_buffer();
  } while (command!=0);

  metis_start_stop(1);

}

static void start_receive_thread() {
  int i;
  int rc;
  struct hostent *h;

  fprintf(stderr,"old_protocol starting receive thread\n");

  DISCOVERED* d=&discovered[selected_device];

  data_socket=socket(PF_INET,SOCK_DGRAM,IPPROTO_UDP);
  if(data_socket<0) {
    perror("old_protocol: create socket failed for data_socket\n");
    exit(-1);
  }

  int optval = 1;
  setsockopt(data_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  // bind to the interface
  if(bind(data_socket,(struct sockaddr*)&d->interface_address,d->interface_length)<0) {
    perror("old_protocol: bind socket failed for data_socket\n");
    exit(-1);
  }

  memcpy(&data_addr,&d->address,d->address_length);
  data_addr_length=d->address_length;
  data_addr.sin_port=htons(DATA_PORT);

  rc=pthread_create(&receive_thread_id,NULL,receive_thread,NULL);
  if(rc != 0) {
    fprintf(stderr,"old_protocol: pthread_create failed on receive_thread: rc=%d\n", rc);
    exit(-1);
  }

}

static void *receive_thread(void* arg) {
  struct sockaddr_in addr;
  int length;
  unsigned char buffer[2048];
  int bytes_read;
  int ep;
  long sequence;

  fprintf(stderr, "old_protocol: receive_thread\n");
  running=1;

  length=sizeof(addr);
  while(running) {
    bytes_read=recvfrom(data_socket,buffer,sizeof(buffer),0,(struct sockaddr*)&addr,&length);
    if(bytes_read<0) {
      perror("recvfrom socket failed for old_protocol: receive_thread");
      exit(1);
    }

    if(buffer[0]==0xEF && buffer[1]==0xFE) {
      switch(buffer[2]) {
        case 1:
          // get the end point
          ep=buffer[3]&0xFF;

          // get the sequence number
          sequence=((buffer[4]&0xFF)<<24)+((buffer[5]&0xFF)<<16)+((buffer[6]&0xFF)<<8)+(buffer[7]&0xFF);

          switch(ep) {
            case 6: // EP6
              // process the data
              process_ozy_input_buffer(&buffer[8]);
              process_ozy_input_buffer(&buffer[520]);
              break;
            case 4: // EP4
/*
              ep4_sequence++;
              if(sequence!=ep4_sequence) {
                ep4_sequence=sequence;
              } else {
                int seq=(int)(sequence%32L);
                if((sequence%32L)==0L) {
                  reset_bandscope_buffer_index();
                }
                process_bandscope_buffer(&buffer[8]);
                process_bandscope_buffer(&buffer[520]);
              }
*/
              break;
            default:
              fprintf(stderr,"unexpected EP %d length=%d\n",ep,bytes_read);
              break;
          }
          break;
        case 2:  // response to a discovery packet
          fprintf(stderr,"unexepected discovery response when not in discovery mode\n");
          break;
        default:
          fprintf(stderr,"unexpected packet type: 0x%02X\n",buffer[2]);
          break;
      }
    } else {
      fprintf(stderr,"received bad header bytes on data port %02X,%02X\n",buffer[0],buffer[1]);
    }
  }
}

static void process_ozy_input_buffer(char  *buffer) {
  int i,j;
  int b=0;
  unsigned char ozy_samples[8*8];
  int bytes;
  int last_ptt;
  int last_dot;
  int last_dash;
  double gain;
  int left_sample;
  int right_sample;
  int mic_sample;
  float left_sample_float;
  float right_sample_float;
  float mic_sample_float;

  if(buffer[b++]==SYNC && buffer[b++]==SYNC && buffer[b++]==SYNC) {
    // extract control bytes
    control_in[0]=buffer[b++];
    control_in[1]=buffer[b++];
    control_in[2]=buffer[b++];
    control_in[3]=buffer[b++];
    control_in[4]=buffer[b++];

    last_ptt=ptt;
    last_dot=dot;
    last_dash=dash;
    ptt=(control_in[0]&0x01)==0x01;
    dash=(control_in[0]&0x02)==0x02;
    dot=(control_in[0]&0x04)==0x04;

    if(last_ptt!=ptt) {
      g_idle_add(ptt_update,(gpointer)ptt);
    }

    switch((control_in[0]>>3)&0x1F) {
      case 0:
        adc_overload=control_in[1]&0x01;
        IO1=(control_in[1]&0x02)?0:1;
        IO2=(control_in[1]&0x04)?0:1;
        IO3=(control_in[1]&0x08)?0:1;
        if(mercury_software_version!=control_in[2]) {
          mercury_software_version=control_in[2];
          fprintf(stderr,"  Mercury Software version: %d (0x%0X)\n",mercury_software_version,mercury_software_version);
        }
        if(penelope_software_version!=control_in[3]) {
          penelope_software_version=control_in[3];
          fprintf(stderr,"  Penelope Software version: %d (0x%0X)\n",penelope_software_version,penelope_software_version);
        }
        if(ozy_software_version!=control_in[4]) {
          ozy_software_version=control_in[4];
          fprintf(stderr,"FPGA firmware version: %d.%d\n",ozy_software_version/10,ozy_software_version%10);
        }
        break;
      case 1:
        exciter_power=((control_in[1]&0xFF)<<8)|(control_in[2]&0xFF); // from Penelope or Hermes
        alex_forward_power=((control_in[3]&0xFF)<<8)|(control_in[4]&0xFF); // from Alex or Apollo
        break;
      case 2:
        alex_reverse_power=((control_in[1]&0xFF)<<8)|(control_in[2]&0xFF); // from Alex or Apollo
        AIN3=(control_in[3]<<8)+control_in[4]; // from Pennelope or Hermes
        break;
      case 3:
        AIN4=(control_in[1]<<8)+control_in[2]; // from Pennelope or Hermes
        AIN6=(control_in[3]<<8)+control_in[4]; // from Pennelope or Hermes
        break;
    }


    // extract the 63 samples
    for(i=0;i<63;i++) {

      left_sample   = (int)((signed char) buffer[b++]) << 16;
      left_sample  += (int)((unsigned char)buffer[b++]) << 8;
      left_sample  += (int)((unsigned char)buffer[b++]);
      right_sample  = (int)((signed char) buffer[b++]) << 16;
      right_sample += (int)((unsigned char)buffer[b++]) << 8;
      right_sample += (int)((unsigned char)buffer[b++]);
      mic_sample    = (int)((signed char) buffer[b++]) << 8;
      mic_sample   += (int)((unsigned char)buffer[b++]);
/*
      left_sample  = ((int)buffer[b++]) << 16;
      left_sample  = (((unsigned char)buffer[b++]) << 8) | left_sample;
      left_sample  = ((unsigned char)buffer[b++]) | left_sample;
      right_sample = ((int)buffer[b++]) << 16;
      right_sample = (((unsigned char)buffer[b++]) << 8) | right_sample;
      right_sample = ((unsigned char)buffer[b++]) | right_sample;
      mic_sample   = ((int)buffer[b++]) << 8;
      mic_sample   = ((unsigned char)buffer[b++]) | mic_sample;
*/
      left_sample_float=(float)left_sample/8388607.0; // 24 bit sample 2^23-1
      right_sample_float=(float)right_sample/8388607.0; // 24 bit sample 2^23-1
      mic_sample_float=(float)mic_sample/32767.0f; // 16 bit sample 2^16-1

      // add to buffer
      if(isTransmitting()) {
        micinputbuffer[samples*2]=(double)mic_sample_float*mic_gain;
        micinputbuffer[(samples*2)+1]=(double)mic_sample_float*mic_gain;
        samples++;
      } else {
        iqinputbuffer[samples*2]=(double)left_sample_float;
        iqinputbuffer[(samples*2)+1]=(double)right_sample_float;
        samples++;
      }

      // when we have enough samples give them to WDSP and get the results
      if(samples==buffer_size) {
        int error;
        if(isTransmitting()) {
          if(tune) {
            double tunefrequency = (double)((filterHigh - filterLow) / 2);
            phase=sineWave(micinputbuffer, BUFFER_SIZE, phase, (float)tunefrequency);
          } else if(mode==modeCWU || mode==modeCWL) {
          }
          // process the output
          fexchange0(CHANNEL_TX, micinputbuffer, micoutputbuffer, &error);
          if(error!=0) {
            fprintf(stderr,"fexchange0 (CHANNEL_TX) returned error: %d\n", error);
          }
          Spectrum0(1, CHANNEL_TX, 0, 0, micoutputbuffer);
          if(penelope) {
            if(tune) {
              gain=65535.0*255.0/(double)tune_drive;
            } else {
              gain=65535.0*255.0/(double)drive;
            }
          } else {
            gain=65535.0;
          }
          for(j=0;j<output_buffer_size;j++) {
            left_rx_sample=0;
            right_rx_sample=0;
            left_tx_sample=(short)(micoutputbuffer[j*2]*gain*2);
            right_tx_sample=(short)(micoutputbuffer[(j*2)+1]*gain*2);
            output_buffer[output_buffer_index++]=left_rx_sample>>8;
            output_buffer[output_buffer_index++]=left_rx_sample;
            output_buffer[output_buffer_index++]=right_rx_sample>>8;
            output_buffer[output_buffer_index++]=right_rx_sample;
            output_buffer[output_buffer_index++]=left_tx_sample>>8;
            output_buffer[output_buffer_index++]=left_tx_sample;
            output_buffer[output_buffer_index++]=right_tx_sample>>8;
            output_buffer[output_buffer_index++]=right_tx_sample;
            if(output_buffer_index>=OZY_BUFFER_SIZE) {
              ozy_send_buffer();
              output_buffer_index=8;
            }
          }
        } else {
          // process the input
          fexchange0(CHANNEL_RX0, iqinputbuffer, audiooutputbuffer, &error);
          if(error!=0) {
            fprintf(stderr,"fexchange2 (CHANNEL_RX0) returned error: %d\n", error);
          }
          Spectrum0(1, CHANNEL_RX0, 0, 0, iqinputbuffer);
          for(j=0;j<output_buffer_size;j++) {
            left_rx_sample=(short)(audiooutputbuffer[j*2]*32767.0*volume);
            right_rx_sample=(short)(audiooutputbuffer[(j*2)+1]*32767.0*volume);
            left_tx_sample=0;
            right_tx_sample=0;
            output_buffer[output_buffer_index++]=left_rx_sample>>8;
            output_buffer[output_buffer_index++]=left_rx_sample;
            output_buffer[output_buffer_index++]=right_rx_sample>>8;
            output_buffer[output_buffer_index++]=right_rx_sample;
            output_buffer[output_buffer_index++]=left_tx_sample>>8;
            output_buffer[output_buffer_index++]=left_tx_sample;
            output_buffer[output_buffer_index++]=right_tx_sample>>8;
            output_buffer[output_buffer_index++]=right_tx_sample;
            if(output_buffer_index>=OZY_BUFFER_SIZE) {
              ozy_send_buffer();
              output_buffer_index=8;
            }
          }
        }
        samples=0;
      }
    }
  } else {
    time_t t;
    struct tm* gmt;
    time(&t);
    gmt=gmtime(&t);

    fprintf(stderr,"%s: process_ozy_input_buffer: did not find sync\n",
            asctime(gmt));
    exit(1);
  }
}

/*
static void process_bandscope_buffer(char  *buffer) {
}
*/


void ozy_send_buffer() {
  output_buffer[0]=SYNC;
  output_buffer[1]=SYNC;
  output_buffer[2]=SYNC;

  switch(command) {
#ifdef EXCLUDE
    case 0:
      //sem_wait(&frequency_changed_sem);
      if(frequencyChanged) {
        // send rx frequency
        output_buffer[3]=control_out[0]|0x04;
        output_buffer[4]=ddsFrequency>>24;
        output_buffer[5]=ddsFrequency>>16;
        output_buffer[6]=ddsFrequency>>8;
        output_buffer[7]=ddsFrequency;
        //freqcommand++;
      } else {
        output_buffer[3]=control_out[0];
        output_buffer[4]=control_out[1];
        output_buffer[5]=control_out[2];
        output_buffer[6]=control_out[3];
        output_buffer[7]=control_out[4];
      }
      //sem_post(&frequency_changed_sem);
      break;
    case 1:
      // send tx frequency
      output_buffer[3]=control_out[0]|0x02;
/*
      if(bSplit) {
        if(frequencyBChanged) {
          output_buffer[3]=control_out[0]|0x02; // Penelope
          output_buffer[4]=ddsBFrequency>>24;
          output_buffer[5]=ddsBFrequency>>16;
          output_buffer[6]=ddsBFrequency>>8;
          output_buffer[7]=ddsBFrequency;
        } else {
          output_buffer[3]=control_out[0];
          output_buffer[4]=control_out[1];
          output_buffer[5]=control_out[2];
          output_buffer[6]=control_out[3];
          output_buffer[7]=control_out[4];
        }
      } else {
*/
        //sem_wait(&frequency_changed_sem);
        if(frequencyChanged) {
          output_buffer[4]=ddsFrequency>>24;
          output_buffer[5]=ddsFrequency>>16;
          output_buffer[6]=ddsFrequency>>8;
          output_buffer[7]=ddsFrequency;
        } else {
          output_buffer[3]=control_out[0];
          output_buffer[4]=control_out[1];
          output_buffer[5]=control_out[2];
          output_buffer[6]=control_out[3];
          output_buffer[7]=control_out[4];
        }
        frequencyChanged=0;
        //sem_post(&frequency_changed_sem);
/*
      }
*/
/*
      frequencyBChanged=0;
*/
      break;
    case 2:
      output_buffer[3]=control_out[0];
      output_buffer[4]=control_out[1];
      output_buffer[5]=control_out[2];
      output_buffer[6]=control_out[3];
      output_buffer[7]=control_out[4];
      break;
#endif

    case 0:
      {
      BAND *band=band_get_current_band();
      output_buffer[3]=control_out[0];
      output_buffer[4]=control_out[1];
      if(isTransmitting()) {
        output_buffer[5]=control_out[2]|band->OCtx;
      } else {
        output_buffer[5]=control_out[2]|band->OCrx;
      }
      output_buffer[6]=control_out[3];
      output_buffer[7]=control_out[4];
      }
      break;
    case 1:
      output_buffer[3]=control_out[0]|0x04;
      output_buffer[4]=ddsFrequency>>24;
      output_buffer[5]=ddsFrequency>>16;
      output_buffer[6]=ddsFrequency>>8;
      output_buffer[7]=ddsFrequency;
      break;
    case 2:
      output_buffer[3]=control_out[0]|0x02;
      output_buffer[4]=ddsFrequency>>24;
      output_buffer[5]=ddsFrequency>>16;
      output_buffer[6]=ddsFrequency>>8;
      output_buffer[7]=ddsFrequency;
      break;
    case 3:
      {
      float d=(float)drive;
      if(tune) {
        d=(float)tune_drive;
      }
      BAND *band=band_get_current_band();
      d=(d/100.0F)*(float)band->pa_calibration;

      output_buffer[3]=0x12;
      output_buffer[4]=(int)d;
      output_buffer[5]=control_out[2];
      if(mic_boost) {
        output_buffer[5]|=0x01;
      }
      if(mic_linein) {
        output_buffer[5]|=0x02;
      }
      if(filter_board==APOLLO) {
        output_buffer[5]|=0x2C; // board, filter ,tuner
      }
      if((filter_board==APOLLO) && tune && apollo_tuner) {
        output_buffer[5]|=0x10;
      }
      output_buffer[6]=control_out[3];
      output_buffer[7]=control_out[4];
      }
      break;
    case 4:
      // need to add orion tip/ring and bias configuration
      output_buffer[3]=0x14;
      output_buffer[4]=0x00;
      if(mic_ptt_enabled==0) {
        output_buffer[4]|=0x40;
      }
      if(mic_bias_enabled) {
        output_buffer[4]|=0x20;
      }
      if(mic_ptt_tip_bias_ring) {
        output_buffer[4]|=0x10;
      }
      output_buffer[5]=0x00;
      output_buffer[6]=0x00;
      output_buffer[7]=0x00;
      break;
    case 5:
      // need to add rx attenuation and cw configuration
      output_buffer[3]=0x16;
      output_buffer[4]=0x00;
      output_buffer[5]=0x00;
      if(cw_keys_reversed!=0) {
        output_buffer[5]|=0x40;
      }
      output_buffer[6]=cw_keyer_speed | (cw_keyer_mode<<6);
      output_buffer[7]=cw_keyer_weight | (cw_keyer_spacing<<7);
      break;
    case 6:
      // need to add tx attenuation and rx ADC selection
      output_buffer[3]=0x1C;
      output_buffer[4]=0x00;
      output_buffer[5]=0x00;
      output_buffer[6]=0x00;
      output_buffer[7]=0x00;
      break;
    case 7:
      // need to add cw configuration
      output_buffer[3]=0x1E;
      if(cw_keyer_internal==1) {
        if(isTransmitting() || (mode!=modeCWU && mode!=modeCWL)) {
          output_buffer[4]=0x00;
        } else {
          output_buffer[4]=0x01;
        }
      } else {
        output_buffer[4]=0x00;
      }
      output_buffer[5]=cw_keyer_sidetone_volume;
      output_buffer[6]=cw_keyer_ptt_delay;
      output_buffer[7]=0x00;
      break;
    case 8:
      // need to add cw configuration
      output_buffer[3]=0x20;
      output_buffer[4]=cw_keyer_hang_time;
      output_buffer[5]=cw_keyer_hang_time>>8;
      output_buffer[6]=cw_keyer_sidetone_frequency;
      output_buffer[7]=cw_keyer_sidetone_frequency>>8;
      break;
  }
  command++;
  //if(command>=14) {
    if(command>=8) {
      command=0;
    }
  // set mox
  output_buffer[3]|=isTransmitting();

  metis_write(0x02,output_buffer,OZY_BUFFER_SIZE);
}

static int metis_write(unsigned char ep,char* buffer,int length) {
  int i;

  // copy the buffer over
  for(i=0;i<512;i++) {
    metis_buffer[i+metis_offset]=buffer[i];
  }

  if(metis_offset==8) {
    metis_offset=520;
  } else {
    send_sequence++;
    metis_buffer[0]=0xEF;
    metis_buffer[1]=0xFE;
    metis_buffer[2]=0x01;
    metis_buffer[3]=ep;
    metis_buffer[4]=(send_sequence>>24)&0xFF;
    metis_buffer[5]=(send_sequence>>16)&0xFF;
    metis_buffer[6]=(send_sequence>>8)&0xFF;
    metis_buffer[7]=(send_sequence)&0xFF;

    // send the buffer
    metis_send_buffer(&metis_buffer[0],1032);
    metis_offset=8;

  }

  return length;
}

static void metis_start_stop(int command) {
  int i;
  unsigned char buffer[64];
    
  buffer[0]=0xEF;
  buffer[1]=0xFE;
  buffer[2]=0x04;    // start/stop command
  buffer[3]=command;    // send EP6 and EP4 data (0x00=stop)

  for(i=0;i<60;i++) {
    buffer[i+4]=0x00;
  }

  metis_send_buffer(buffer,sizeof(buffer));
}

static void metis_send_buffer(char* buffer,int length) {
  if(sendto(data_socket,buffer,length,0,(struct sockaddr*)&data_addr,data_addr_length)!=length) {
    perror("sendto socket failed for metis_send_data\n");
  }
}


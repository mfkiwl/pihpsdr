/* Copyright (C)
* 2019 - John Melton, G0ORX/N6LYT
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wdsp.h>

#include "SoapySDR/Constants.h"
#include "SoapySDR/Device.h"
#include "SoapySDR/Formats.h"
#include "SoapySDR/Version.h"

//#define TIMING
#ifdef TIMING
#include <sys/time.h>
#endif

#include "band.h"
#include "channel.h"
#include "discovered.h"
#include "mode.h"
#include "filter.h"
#include "receiver.h"
#include "transmitter.h"
//#include "wideband.h"
//#include "adc.h"
//#include "dac.h"
#include "radio.h"
#include "main.h"
//#include "protocol1.h"
#include "soapy_protocol.h"
#include "audio.h"
#include "signal.h"
#include "vfo.h"
#include "ext.h"
#include "error_handler.h"

static double bandwidth=2000000.0;

static SoapySDRDevice *soapy_device;
static SoapySDRStream *rx_stream;
static SoapySDRStream *tx_stream;
static int max_samples;

static int samples=0;

static GThread *receive_thread_id;
static gpointer receive_thread(gpointer data);

static int actual_rate;

#ifdef TIMING
static int rate_samples;
#endif

static gboolean running;

static int mic_samples=0;
static int mic_sample_divisor=1;

static int max_tx_samples;
static float *output_buffer;
static int output_buffer_index;

SoapySDRDevice *get_soapy_device() {
  return soapy_device;
}

void soapy_protocol_set_mic_sample_rate(int rate) {
  mic_sample_divisor=rate/48000;
}

void soapy_protocol_change_sample_rate(RECEIVER *rx) {
// rx->mutex already locked
  if(rx->sample_rate==radio_sample_rate) {
    if(rx->resample_buffer!=NULL) {
      g_free(rx->resample_buffer);
      rx->resample_buffer=NULL;
      rx->resample_buffer_size=0;
    }
    if(rx->resampler!=NULL) {
      destroy_resample(rx->resampler);
      rx->resampler=NULL;
    }
  } else {
    if(rx->resample_buffer!=NULL) {
      g_free(rx->resample_buffer);
      rx->resample_buffer=NULL;
    }
    if(rx->resampler!=NULL) {
      destroy_resample(rx->resampler);
      rx->resampler=NULL;
    }
    rx->resample_buffer_size=2*max_samples/(radio_sample_rate/rx->sample_rate);
    rx->resample_buffer=g_new(double,rx->resample_buffer_size);
    rx->resampler=create_resample (1,max_samples,rx->buffer,rx->resample_buffer,radio_sample_rate,rx->sample_rate,0.0,0,1.0);

  }

}

void soapy_protocol_create_receiver(RECEIVER *rx) {
  int rc;

  mic_sample_divisor=rx->sample_rate/48000;

fprintf(stderr,"soapy_protocol_create_receiver: setting samplerate=%f adc=%d mic_sample_divisor=%d\n",(double)radio_sample_rate,rx->adc,mic_sample_divisor);
  rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_RX,rx->adc,(double)radio_sample_rate);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setSampleRate(%f) failed: %s\n",(double)radio_sample_rate,SoapySDR_errToStr(rc));
  }

  size_t channel=rx->adc;
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setupStream(version<0x00080000): channel=%ld\n",channel);
  rc=SoapySDRDevice_setupStream(soapy_device,&rx_stream,SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setupStream (RX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#else
fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setupStream(version>=0x00080000): channel=%ld\n",channel);
  rx_stream=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_RX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rx_stream==NULL) {
    fprintf(stderr,"soapy_protocol_create_receiver: SoapySDRDevice_setupStream (RX) failed (rx_stream is NULL)\n");
    _exit(-1);
  }
#endif


  max_samples=SoapySDRDevice_getStreamMTU(soapy_device,rx_stream);
  if(max_samples>(2*rx->fft_size)) {
    max_samples=2*rx->fft_size;
  }
  if(max_samples>=4096) {
    max_samples=4096;
  } else if(max_samples>=2048) {
    max_samples=2048;
  } else {
    max_samples=1024;
  }
  rx->buffer=g_new(double,max_samples*2);

  if(rx->sample_rate==radio_sample_rate) {
    rx->resample_buffer=NULL;
    rx->resampler=NULL;
    rx->resample_buffer_size=0;
  } else {
    rx->resample_buffer_size=2*max_samples/(radio_sample_rate/rx->sample_rate);
    rx->resample_buffer=g_new(double,rx->resample_buffer_size);
    rx->resampler=create_resample (1,max_samples,rx->buffer,rx->resample_buffer,radio_sample_rate,rx->sample_rate,0.0,0,1.0);
  }


fprintf(stderr,"soapy_protocol_create_receiver: max_samples=%d buffer=%p\n",max_samples,rx->buffer);

}

void soapy_protocol_start_receiver(RECEIVER *rx) {
  int rc;

double rate=SoapySDRDevice_getSampleRate(soapy_device,SOAPY_SDR_RX,rx->adc);
fprintf(stderr,"soapy_protocol_start_receiver: activate_stream rate=%f\n",rate);
  rc=SoapySDRDevice_activateStream(soapy_device, rx_stream, 0, 0LL, 0);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_start_receiver: SoapySDRDevice_activateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }

fprintf(stderr,"soapy_protocol_start_receiver: create receive_thread\n");
  receive_thread_id = g_thread_new( "soapy_rx", receive_thread, rx);
  if( ! receive_thread_id )
  {
    fprintf(stderr,"g_thread_new failed for receive_thread\n");
    exit( -1 );
  }
  fprintf(stderr, "receive_thread: id=%p\n",receive_thread_id);
}

void soapy_protocol_create_transmitter(TRANSMITTER *tx) {
  int rc;


fprintf(stderr,"soapy_protocol_create_transmitter: setting samplerate=%f\n",(double)tx->iq_output_rate);
  rc=SoapySDRDevice_setSampleRate(soapy_device,SOAPY_SDR_TX,tx->dac,(double)tx->iq_output_rate);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_configure_transmitter: SoapySDRDevice_setSampleRate(%f) failed: %s\n",(double)tx->iq_output_rate,SoapySDR_errToStr(rc));
  }


  size_t channel=tx->dac;
fprintf(stderr,"soapy_protocol_create_transmitter: SoapySDRDevice_setupStream: channel=%ld\n",channel);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc=SoapySDRDevice_setupStream(soapy_device,&tx_stream,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (RX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#else
  tx_stream=SoapySDRDevice_setupStream(soapy_device,SOAPY_SDR_TX,SOAPY_SDR_CF32,&channel,1,NULL);
  if(tx_stream==NULL) {
    fprintf(stderr,"soapy_protocol_create_transmitter: SoapySDRDevice_setupStream (TX) failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
#endif

  max_tx_samples=SoapySDRDevice_getStreamMTU(soapy_device,tx_stream);
  if(max_tx_samples>(2*tx->fft_size)) {
    max_tx_samples=2*tx->fft_size;
  }
fprintf(stderr,"soapy_protocol_create_transmitter: max_tx_samples=%d\n",max_tx_samples);
  output_buffer=(float *)malloc(max_tx_samples*sizeof(float)*2);

}

void soapy_protocol_start_transmitter(TRANSMITTER *tx) {
  int rc;

double rate=SoapySDRDevice_getSampleRate(soapy_device,SOAPY_SDR_TX,tx->dac);
fprintf(stderr,"soapy_protocol_start_transmitter: activateStream rate=%f\n",rate);
  rc=SoapySDRDevice_activateStream(soapy_device, tx_stream, 0, 0LL, 0);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_start_transmitter: SoapySDRDevice_activateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
}

void soapy_protocol_stop_transmitter(TRANSMITTER *tx) {
  int rc;

fprintf(stderr,"soapy_protocol_stop_transmitter: deactivateStream\n");
  rc=SoapySDRDevice_deactivateStream(soapy_device, tx_stream, 0, 0LL);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol_stop_transmitter: SoapySDRDevice_deactivateStream failed: %s\n",SoapySDR_errToStr(rc));
    _exit(-1);
  }
}

void soapy_protocol_init(int rx,gboolean hf) {
  SoapySDRKwargs args={};
  int rc;
  int i;

fprintf(stderr,"soapy_protocol_init: rx=%d hf=%d\n",rx,hf);

  // initialize the radio
fprintf(stderr,"soapy_protocol_init: SoapySDRDevice_make\n");
  SoapySDRKwargs_set(&args, "driver", radio->name);
  if(strcmp(radio->name,"rtlsdr")==0) {
    char id[16];
    sprintf(id,"%d",radio->info.soapy.rtlsdr_count);
    SoapySDRKwargs_set(&args, "rtl", id);

    if(hf) {
      SoapySDRKwargs_set(&args, "direct_samp", "2");
    } else {
      SoapySDRKwargs_set(&args, "direct_samp", "0");
    }
  }
  soapy_device=SoapySDRDevice_make(&args);
  if(soapy_device==NULL) {
    fprintf(stderr,"soapy_protocol: SoapySDRDevice_make failed: %s\n",SoapySDRDevice_lastError());
    _exit(-1);
  }
  SoapySDRKwargs_clear(&args);

  if(can_transmit) {
    if(transmitter->local_microphone) {
      if(audio_open_input()!=0) {
        fprintf(stderr,"audio_open_input failed\n");
        transmitter->local_microphone=0;
      }
    }
  }

}

static void *receive_thread(void *arg) {
  double isample;
  double qsample;
  int elements;
  int flags=0;
  long long timeNs=0;
  long timeoutUs=100000L;
  int i;
  RECEIVER *rx=(RECEIVER *)arg;
  float *buffer=g_new(float,max_samples*2);
  void *buffs[]={buffer};
  float fsample;
  running=TRUE;
fprintf(stderr,"soapy_protocol: receive_thread\n");
  while(running) {
    elements=SoapySDRDevice_readStream(soapy_device,rx_stream,buffs,max_samples,&flags,&timeNs,timeoutUs);
    //fprintf(stderr,"soapy_protocol_receive_thread: SoapySDRDevice_readStream failed: max_samples=%d read=%d\n",max_samples,elements);
    if(elements<0) {
      continue;
    }
    
    for(i=0;i<elements;i++) {
      rx->buffer[i*2]=(double)buffer[i*2];
      rx->buffer[(i*2)+1]=(double)buffer[(i*2)+1];
    }

    if(rx->resampler!=NULL) {
      int samples=xresample(rx->resampler);
      for(i=0;i<samples;i++) {
        isample=rx->resample_buffer[i*2];
        qsample=rx->resample_buffer[(i*2)+1];
        if(iqswap) {
          add_iq_samples(rx,qsample,isample);
        } else {
          add_iq_samples(rx,isample,qsample);
        }
        if(can_transmit) {
          mic_samples++;
          if(mic_samples>=mic_sample_divisor) { // reduce to 48000
            if(transmitter!=NULL) {
              fsample = transmitter->local_microphone ? audio_get_next_mic_sample() : 0.0F;
            } else {
              fsample=0.0F;
            }
            add_mic_sample(transmitter,fsample);
            mic_samples=0;
          }
        }
      }
    } else {
      for(i=0;i<elements;i++) {
        isample=rx->buffer[i*2];
        qsample=rx->buffer[(i*2)+1];
        if(iqswap) {
          add_iq_samples(rx,qsample,isample);
        } else {
          add_iq_samples(rx,isample,qsample);
        }
        if(can_transmit) {
          mic_samples++;
          if(mic_samples>=mic_sample_divisor) { // reduce to 48000
            if(transmitter!=NULL) {
              fsample = transmitter->local_microphone ? audio_get_next_mic_sample() : 0.0F;
            } else {
              fsample=0.0F;
            }
            add_mic_sample(transmitter,fsample);
            mic_samples=0;
          }
        }
      }
    }
  }

fprintf(stderr,"soapy_protocol: receive_thread: SoapySDRDevice_deactivateStream\n");
  SoapySDRDevice_deactivateStream(soapy_device,rx_stream,0,0LL);
fprintf(stderr,"soapy_protocol: receive_thread: SoapySDRDevice_closeStream\n");
  SoapySDRDevice_closeStream(soapy_device,rx_stream);
fprintf(stderr,"soapy_protocol: receive_thread: SoapySDRDevice_unmake\n");
  SoapySDRDevice_unmake(soapy_device);
  return NULL;
}

void soapy_protocol_process_local_mic(float sample) {
  add_mic_sample(transmitter,sample);
}

void soapy_protocol_iq_samples(float isample,float qsample) {
  const void *tx_buffs[]={output_buffer};
  int flags=0;
  long long timeNs=0;
  long timeoutUs=100000L;
  if(isTransmitting()) {
    output_buffer[(output_buffer_index*2)]=isample;
    output_buffer[(output_buffer_index*2)+1]=qsample;
    output_buffer_index++;
    if(output_buffer_index>=max_tx_samples) {
      int elements=SoapySDRDevice_writeStream(soapy_device,tx_stream,tx_buffs,max_tx_samples,&flags,timeNs,timeoutUs);
      if(elements!=max_tx_samples) {
        g_print("soapy_protocol_iq_samples: writeStream returned %d for %d elements\n",elements,max_tx_samples);
      }
      output_buffer_index=0;
    }
  }
}



void soapy_protocol_stop() {
fprintf(stderr,"soapy_protocol_stop\n");
  running=FALSE;
}

void soapy_protocol_set_rx_frequency(RECEIVER *rx,int v) {
  int rc;

  if(soapy_device!=NULL) {
    double f=(double)(vfo[v].frequency-vfo[v].lo);
    rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_RX,rx->adc,f,NULL);
    if(rc!=0) {
      fprintf(stderr,"soapy_protocol: SoapySDRDevice_setFrequency(RX) failed: %s\n",SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_tx_frequency(TRANSMITTER *tx) {
  int v;
  int rc;
  double f;

  v=get_tx_vfo();
  if(soapy_device!=NULL) {
    if(vfo[v].ctun) {
      f=(double)(vfo[v].ctun_frequency);
    } else {
      f=(double)(vfo[v].frequency);
    }

    if(transmitter->xit_enabled) {
      f+=(double)(transmitter->xit);
    }

//fprintf(stderr,"soapy_protocol_set_tx_frequency: %f\n",f);
    rc=SoapySDRDevice_setFrequency(soapy_device,SOAPY_SDR_TX,tx->dac,f,NULL);
    if(rc!=0) {
      fprintf(stderr,"soapy_protocol: SoapySDRDevice_setFrequency(TX) failed: %s\n",SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_rx_antenna(RECEIVER *rx,int ant) {
  int rc;
  if(soapy_device!=NULL) {
    if (ant >= radio->info.soapy.rx_antennas) ant=radio->info.soapy.rx_antennas -1;
    g_print("soapy_protocol: set_rx_antenna: %s\n",radio->info.soapy.rx_antenna[ant]);
    rc=SoapySDRDevice_setAntenna(soapy_device,SOAPY_SDR_RX,rx->adc,radio->info.soapy.rx_antenna[ant]);
    if(rc!=0) {
      fprintf(stderr,"soapy_protocol: SoapySDRDevice_setAntenna RX failed: %s\n",SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_tx_antenna(TRANSMITTER *tx,int ant) {
  int rc;
  if(soapy_device!=NULL) {
    if (ant >= radio->info.soapy.tx_antennas) ant=radio->info.soapy.tx_antennas -1;
    g_print("soapy_protocol: set_tx_antenna: %s\n",radio->info.soapy.tx_antenna[ant]);
    rc=SoapySDRDevice_setAntenna(soapy_device,SOAPY_SDR_TX,tx->dac,radio->info.soapy.tx_antenna[ant]);
    if(rc!=0) {
      fprintf(stderr,"soapy_protocol: SoapySDRDevice_setAntenna TX failed: %s\n",SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_gain(RECEIVER *rx,double gain) {
  int rc;
//fprintf(stderr,"soapy_protocol_set_gain: adc=%d gain=%f\n",gain);
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_RX,rx->adc,gain);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol: SoapySDRDevice_setGain failed: %s\n",SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_set_gain_element(RECEIVER *rx,char *name,int gain) {
  int rc;
//fprintf(stderr,"soapy_protocol_set_gain: adc=%d %s=%d\n",rx->adc,name,gain);
  rc=SoapySDRDevice_setGainElement(soapy_device,SOAPY_SDR_RX,rx->adc,name,(double)gain);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol: SoapySDRDevice_setGainElement %s failed: %s\n",name,SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_set_tx_gain(TRANSMITTER *tx,int gain) {
  int rc;
  rc=SoapySDRDevice_setGain(soapy_device,SOAPY_SDR_TX,tx->dac,(double)gain);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol: SoapySDRDevice_setGain failed: %s\n",SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_set_tx_gain_element(TRANSMITTER *tx,char *name,int gain) {
  int rc;
  rc=SoapySDRDevice_setGainElement(soapy_device,SOAPY_SDR_TX,tx->dac,name,(double)gain);
  if(rc!=0) {
    fprintf(stderr,"soapy_protocol: SoapySDRDevice_setGainElement %s failed: %s\n",name,SoapySDR_errToStr(rc));
  }
}

int soapy_protocol_get_gain_element(RECEIVER *rx,char *name) {
  double gain;
  gain=SoapySDRDevice_getGainElement(soapy_device,SOAPY_SDR_RX,rx->adc,name);
  return (int)gain;
}

int soapy_protocol_get_tx_gain_element(TRANSMITTER *tx,char *name) {
  double gain;
  gain=SoapySDRDevice_getGainElement(soapy_device,SOAPY_SDR_TX,tx->dac,name);
  return (int)gain;
}

gboolean soapy_protocol_get_automatic_gain(RECEIVER *rx) {
  gboolean mode=SoapySDRDevice_getGainMode(soapy_device, SOAPY_SDR_RX, rx->adc);
  return mode;
}

void soapy_protocol_set_automatic_gain(RECEIVER *rx,gboolean mode) {
  int rc;
  rc=SoapySDRDevice_setGainMode(soapy_device, SOAPY_SDR_RX, rx->adc,mode);
  if(rc!=0) {

    fprintf(stderr,"soapy_protocol: SoapySDRDevice_getGainMode failed: %s\n", SoapySDR_errToStr(rc));
  }
}

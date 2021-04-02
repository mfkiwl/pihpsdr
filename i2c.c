#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/i2c-dev.h>
//#include <i2c/smbus.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <gtk/gtk.h>
//#include "i2c.h"
#include "wiringPiI2C.h"
#include "gpio.h"
#include "band.h"
#include "band_menu.h"
#include "bandstack.h"
#include "radio.h"
#include "toolbar.h"
#include "vfo.h"
#include "ext.h"

char *i2c_device="/dev/i2c-1";
unsigned int i2c_address_1=0X20;
unsigned int i2c_address_2=0X23;

static int fd;

#define SW_2  0X8000
#define SW_3  0X4000
#define SW_4  0X2000
#define SW_5  0X1000
#define SW_6  0X0008
#define SW_7  0X0004
#define SW_8  0X0002
#define SW_9  0X0001
#define SW_10 0X0010
#define SW_11 0X0020
#define SW_12 0X0040
#define SW_13 0X0080
#define SW_14 0X0800
#define SW_15 0X0400
#define SW_16 0X0200
#define SW_17 0X0100

unsigned int i2c_sw[16]=
    { SW_2,SW_3,SW_4,SW_5,SW_6,SW_7,SW_8,SW_9,
      SW_10,SW_11,SW_12,SW_13,SW_14,SW_15,SW_16,SW_17 };

static int write_byte_data(unsigned char addr,unsigned char reg, unsigned char data) {
  int rc;

  rc=wiringPiI2CWriteReg8(fd,reg,data);
  
  return 0;
}

static unsigned char read_byte_data(unsigned char addr,unsigned char reg) {
  int rc;

  rc=wiringPiI2CReadReg8(fd,reg);

  return rc;
}

static unsigned int read_word_data(unsigned char addr,unsigned char reg) {
  int rc;

  rc=wiringPiI2CReadReg16(fd,reg);

  return rc;
}


static void frequencyStep(int pos) {
  vfo_step(pos);
}

void i2c_interrupt() {
  unsigned int flags;
  unsigned int ints;

  do {
    flags=read_word_data(i2c_address_1,0x0E);
    if(flags) {
      ints=read_word_data(i2c_address_1,0x10);
//g_print("i2c_interrupt: flags=%04X ints=%04X\n",flags,ints);
      if(ints) {
        int i;
        for(i=0;i<16;i++) {
          if(i2c_sw[i]==ints) break;
        }
        if(i<16) {
//g_print("i1c_interrupt: sw=%d action=%d\n",i,sw_action[i]);
          switch(sw_action[i]) {
            case TUNE:
              if(can_transmit) {
                int tune=getTune();
                if(tune==0) tune=1; else tune=0;
                  g_idle_add(ext_tune_update,GINT_TO_POINTER(tune));
              }
              break;
            case MOX:
              if(can_transmit) {
                int mox=getMox();
                if(mox==0) mox=1; else mox=0;
                g_idle_add(ext_mox_update,GINT_TO_POINTER(mox));
              }
              break;
            case PS:
#ifdef PURESIGNAL
              if(can_transmit) g_idle_add(ext_ps_update,NULL);
#endif
              break;
            case TWO_TONE:
              if(can_transmit) g_idle_add(ext_two_tone,NULL);
              break;
            case NR:
              g_idle_add(ext_nr_update,NULL);
              break;
            case NB:
              g_idle_add(ext_nb_update,NULL);
              break;
            case SNB:
              g_idle_add(ext_snb_update,NULL);
              break;
            case RIT:
              g_idle_add(ext_rit_update,NULL);
              break;
            case RIT_CLEAR:
              g_idle_add(ext_rit_clear,NULL);
              break;
            case XIT:
              if(can_transmit) g_idle_add(ext_xit_update,NULL);
              break;
            case XIT_CLEAR:
              if(can_transmit) g_idle_add(ext_xit_clear,NULL);
              break;
            case BAND_PLUS:
              g_idle_add(ext_band_plus,NULL);
              break;
            case BAND_MINUS:
              g_idle_add(ext_band_minus,NULL);
              break;
            case BANDSTACK_PLUS:
              g_idle_add(ext_bandstack_plus,NULL);
              break;
            case BANDSTACK_MINUS:
              g_idle_add(ext_bandstack_minus,NULL);
              break;
            case MODE_PLUS:
              g_idle_add(ext_mode_plus,NULL);
              break;
            case MODE_MINUS:
              g_idle_add(ext_mode_minus,NULL);
              break;
            case FILTER_PLUS:
              g_idle_add(ext_filter_plus,NULL);
              break;
            case FILTER_MINUS:
              g_idle_add(ext_filter_minus,NULL);
              break;
            case A_TO_B:
              g_idle_add(ext_vfo_a_to_b,NULL);
              break;
            case B_TO_A:
              g_idle_add(ext_vfo_b_to_a,NULL);
              break;
            case A_SWAP_B:
              g_idle_add(ext_vfo_a_swap_b,NULL);
              break;
            case LOCK:
              g_idle_add(ext_lock_update,NULL);
              break;
            case CTUN:
              g_idle_add(ext_ctun_update,NULL);
              break;
            case AGC:
              g_idle_add(ext_agc_update,NULL);
              break;
            case SPLIT:
              if(can_transmit) g_idle_add(ext_split_toggle,NULL);
              break;
            case DIVERSITY:
              g_idle_add(ext_diversity_update,GINT_TO_POINTER(0));
              break;
            case SAT:
              if(can_transmit) g_idle_add(ext_sat_update,NULL);
              break;
            case MENU_BAND:
              g_idle_add(ext_band_update,NULL);
              break;
            case MENU_BANDSTACK:
              g_idle_add(ext_bandstack_update,NULL);
              break;
            case MENU_MODE:
              g_idle_add(ext_mode_update,NULL);
              break;
            case MENU_FILTER:
              g_idle_add(ext_filter_update,NULL);
              break;
            case MENU_FREQUENCY:
              g_idle_add(ext_frequency_update,NULL);
              break;
            case MENU_MEMORY:
              g_idle_add(ext_memory_update,NULL);
              break;
            case MENU_DIVERSITY:
              g_idle_add(ext_diversity_update,GINT_TO_POINTER(1));
              break;
            case MENU_PS:
#ifdef PURESIGNAL
              g_idle_add(ext_start_ps,NULL);
#endif
              break;
            case FUNCTION:
              g_idle_add(ext_function_update,NULL);
              break;
            case MUTE:
              g_idle_add(ext_mute_update,NULL);
              break;
            case PAN_MINUS:
              g_idle_add(ext_pan_update,GINT_TO_POINTER(-100));
              break;
            case PAN_PLUS:
              g_idle_add(ext_pan_update,GINT_TO_POINTER(100));
              break;
            case ZOOM_MINUS:
              g_idle_add(ext_zoom_update,GINT_TO_POINTER(-1));
              break;
            case ZOOM_PLUS:
              g_idle_add(ext_zoom_update,GINT_TO_POINTER(1));
              break;
          }
        }
      }
    }
  } while(flags!=0);
}

void i2c_init() {

  int flags, ints;

fprintf(stderr,"i2c_init: %s\n",i2c_device);

  fd=wiringPiI2CSetupInterface(i2c_device, i2c_address_1);
  if(fd<0) {
    g_print("i2c_init failed: fd=%d\n",fd);
    return;
  }

  // setup i2c
  if(write_byte_data(i2c_address_1,0x0A,0x44)<0) return;
  if(write_byte_data(i2c_address_1,0x0B,0x44)<0) return;

  // disable interrupt
  if(write_byte_data(i2c_address_1,0x04,0x00)<0) return;
  if(write_byte_data(i2c_address_1,0x05,0x00)<0) return;

  // clear defaults
  if(write_byte_data(i2c_address_1,0x06,0x00)<0) return;
  if(write_byte_data(i2c_address_1,0x07,0x00)<0) return;

  // OLAT
  if(write_byte_data(i2c_address_1,0x14,0x00)<0) return;
  if(write_byte_data(i2c_address_1,0x15,0x00)<0) return;

  // set GPIOA for pullups
  if(write_byte_data(i2c_address_1,0x0C,0xFF)<0) return;
  if(write_byte_data(i2c_address_1,0x0D,0xFF)<0) return;

  // reverse polarity
  if(write_byte_data(i2c_address_1,0x02,0xFF)<0) return;
  if(write_byte_data(i2c_address_1,0x03,0xFF)<0) return;

  // set GPIOA/B for input
  if(write_byte_data(i2c_address_1,0x00,0xFF)<0) return;
  if(write_byte_data(i2c_address_1,0x01,0xFF)<0) return;

  // INTCON
  if(write_byte_data(i2c_address_1,0x08,0x00)<0) return;
  if(write_byte_data(i2c_address_1,0x09,0x00)<0) return;

  // setup for an MCP23017 interrupt
  if(write_byte_data(i2c_address_1,0x04,0xFF)<0) return;
  if(write_byte_data(i2c_address_1,0x05,0xFF)<0) return;

  // flush any interrupts
  int count=0;
  do {
    flags=read_word_data(i2c_address_1,0x0E);
    if(flags) {
      ints=read_word_data(i2c_address_1,0x10);
      fprintf(stderr,"flush interrupt: flags=%04X ints=%04X\n",flags,ints);
      count++;
      if(count==10) {
        return;
      }
    }
  } while(flags!=0);
  
}

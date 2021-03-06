/**********************************************************************
 * Copyright (C) 2012 Al Niessner
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110, USA
 *
 *====================================================================
 *
 * Module Description:
 *
 * read/write use null terminated strings. In both cases they are limited
 * to len if the terminator is not found before then. read returns the
 * actual length of the string not including the terminator.
 *
 *********************************************************************/

#include <delays.h>
#include <spi.h>

#include "HardwareProfile.h"
#include "memory.h"
#include "memory_eeprom.h"
#include "sdcard.h"

#define SD_PAGE_SIZE (0x200u)

typedef enum { SD_GO_IDLE_STATE    = 0x40,// CMD0   0x00
               SD_SEND_IF_COND     = 0x48,// CMD8
               SD_SEND_CSD         = 0x49,// CMD9
               SD_SEND_CID         = 0x4a,// CMD10
               SD_SEND_STATUS      = 0x4d,// CMD13
               SD_SET_BLOCKLEN     = 0x50,// CMD16
               SD_READ_BLOCK       = 0x51,// CMD17
               SD_WRITE_BLOCK      = 0x58,// CMD24
               SD_APP_CMD          = 0x77,// CMD55
               SD_READ_OCR         = 0x7a,// CMD58
               SD_APP_SEND_OP_COND = 0x69,// ACMD41 0x29
               SD_NULL             = 0xff // with bit 7 set, command is invalid
               } sd_command_t;
typedef enum { R1, R1b, R2, R3, R7 } sd_response_name_t;

typedef struct
{
  unsigned idle          :1;
  unsigned erase_reset   :1;
  unsigned illegal_cmd   :1;
  unsigned cmd_crc_err   :1;
  unsigned erase_seq_err :1;
  unsigned addr_err      :1;
  unsigned param_err     :1;
  unsigned valid         :1;
} r1_t;

typedef union
{
  unsigned char _byte[2];
  unsigned int  value;
} sd_crc16_t;

typedef union
{
  unsigned char val[5];
  struct // R1 and R1b
  {
    r1_t r1;
    unsigned unused_r1[4];
  };

  struct // R2
  {
    unsigned int r2_val;
    unsigned unused_r2[3];
  };

  struct // R3
  {
    r1_t r1;
    unsigned long int ocr;
  };

  struct // R7
  {
    r1_t r1;
    struct
    {
      unsigned resv : 4;
      unsigned cver : 4;
    } high;
    struct
    {
      unsigned volt : 4;
      unsigned resv : 4;
    } low;
    unsigned char echo;
  };
} sd_response_t;

typedef union
{
  unsigned char _byte[25];
  struct
  {
    unsigned char     mid;
    unsigned long int sn;

    unsigned char     read_crc_1;
    unsigned long int read_page_1;

    unsigned char     read_crc_2;
    unsigned long int read_page_2;

    unsigned char     write_crc_1;
    unsigned long int write_page_1;

    unsigned char     write_crc_2;
    unsigned long int write_page_2;
  };
} sd_mbr_t;


#pragma udata access fast_access
static near sd_crc16_t   crc, expected;
static near unsigned int bidx, offset;

#pragma udata overlay gps_sdcard
static sdcard_shared_block_t sdcard;

#pragma udata

static sd_command_t      cmd;
static sd_mbr_t          mbr;
static sd_response_t     reply;
static unsigned char     version;
static unsigned long int arg;


#pragma code

/* Because the C18 compiler does not understand pragma inline, need to use
 * directives. I am not a fan of this and it is going to require a lot of
 * effort on my part to get the macros correct.
 */
#define sdcard_broadcast(a,b,c) sdcard.step=a; \
                                sdcard.last_r1=b; \
                                sdcard.ver=c;
#define sdcard_broadcast_xfer(a,b,c) sdcard.isReading=a; \
                                     sdcard.block_r1=a; \
                                     sdcard.isValidCRC;

unsigned char sdcard_crc7 (unsigned char data, unsigned char prev)
{
  static unsigned char crc, i;

  crc = prev;
  for (i = 0 ; i < 8u ; i++)
    {
      crc = crc << 1;
      if ((data ^ crc) & 0x80) crc = crc ^ 0x09;
      data = data << 1;

    }

  return crc;
}

unsigned int sdcard_crc16 (unsigned char data, unsigned int crc) 
{ 
  static unsigned int x;

  x = (crc >> 8) & 0x00ff;
  x = x ^ data;
  x = x ^ (x >> 4); 
  crc = (crc << 8) ^ ((x << 8) << 4) ^ ((x << 4) << 1) ^ x; 

  return crc; 
}


void sdcard_load_mbr (void)
{
  static bool_t        mbr_b;
  static unsigned char crcv[4];
  static unsigned char mbr_i;

  sdcard.read_page  = 0;
  sdcard.write_page = 0;
  for (mbr_i = 0 ; mbr_i < sizeof (sd_mbr_t) ; mbr_i++) 
    mbr._byte[mbr_i] = eeprom_read (SD_CIRC_BUFF_INDICES_ADDR + mbr_i);
  mbr_b = mbr._byte[0] == sdcard.cid[0];
  for (mbr_i = 1 ; mbr_i < 5u ; mbr_i++)
    mbr_b &= mbr._byte[mbr_i] == sdcard.cid[mbr_i + 8];

  if (mbr_b)
    {
      crcv[0] = crcv[1] = crcv[2] = crcv[3] = 0;
      for (mbr_i = 1 ; mbr_i < 5u ; mbr_i++)
        {
          crcv[0] = sdcard_crc7 (mbr._byte[ 5 + mbr_i], crcv[0]);
          crcv[1] = sdcard_crc7 (mbr._byte[10 + mbr_i], crcv[1]);
          crcv[2] = sdcard_crc7 (mbr._byte[15 + mbr_i], crcv[2]);
          crcv[3] = sdcard_crc7 (mbr._byte[20 + mbr_i], crcv[3]);
        }

      if (mbr.read_crc_1 == mbr.read_crc_2 &&
          mbr.read_crc_1 ==     crcv[0]       ) sdcard.read_page = mbr.read_page_1;
      else
        {
          if (crcv[0] == mbr.read_crc_1 &&
              crcv[1] != mbr.read_crc_2    ) sdcard.read_page = mbr.read_page_1;

          if (crcv[0] != mbr.read_crc_1 &&
              crcv[1] == mbr.read_crc_2    ) sdcard.read_page = mbr.read_page_2;

          if (crcv[0] == mbr.read_crc_1 &&
              crcv[1] == mbr.read_crc_2    )
            {
              if (mbr.read_page_1 + 1 == mbr.read_page_2)
                sdcard.read_page = mbr.read_page_2;
              else sdcard.read_page = mbr.read_page_1;
            }
        }

      if (mbr.write_crc_1 == mbr.write_crc_2 &&
          mbr.write_crc_1 ==     crcv[2]        ) sdcard.write_page = mbr.write_page_1;
      else
        {
          if (crcv[2] == mbr.write_crc_1 &&
              crcv[3] != mbr.write_crc_2    ) sdcard.write_page = mbr.write_page_1;

          if (crcv[2] != mbr.write_crc_1 &&
              crcv[3] == mbr.write_crc_2    ) sdcard.write_page = mbr.write_page_2;

          if (crcv[2] == mbr.write_crc_1 &&
              crcv[3] == mbr.write_crc_2    )
            {
              if (mbr.write_page_1 + 1 == mbr.write_page_2)
                sdcard.write_page = mbr.write_page_2;
              else sdcard.write_page = mbr.write_page_1;
            }
        }
    }
  else
    {
      // save off current SD Card information for next boot cycle
      eeprom_write (SD_CIRC_BUFF_INDICES_ADDR, sdcard.cid[0]);
      for (mbr_i = 1 ; mbr_i < 5u ; mbr_i++)
        eeprom_write (SD_CIRC_BUFF_INDICES_ADDR + mbr_i, sdcard.cid[8 + mbr_i]);
    }
}

void sdcard_process (sd_response_name_t r)
{
  static unsigned char crc,data,i,j,n;

  SD_CS = 0;
  putcSPI (cmd);
  crc = sdcard_crc7 (cmd, 0);
  data = (arg >> 24) & 0xff;
  putcSPI (data);
  crc = sdcard_crc7 (data, crc);
  data = (arg >> 16) & 0xff;
  putcSPI (data);
  crc = sdcard_crc7 (data, crc);
  data = (arg >> 8) & 0xff;
  putcSPI (data);
  crc = sdcard_crc7 (data, crc);
  data =  arg & 0xff;
  putcSPI (data);
  crc = sdcard_crc7 (data, crc);
  crc = (crc << 1) | 1;
  putcSPI (crc);
  for (i = 0 ; i < 5u ;  i++) reply.val[i] = SD_NULL;
  switch (r)
    {
    case R1:
    case R1b:
      n = 1;
      break;

    case R2:
      n = 2;
      break;

    case R3:
    case R7:
      n = 5;
      break;
    }
  for (i = 0x09 ; i && reply.val[0] == SD_NULL ; i--) //Ncr delay
    {
      putcSPI (SD_NULL); // keep sclk moving
      reply.val[0] = getcSPI();
    }
  for (j = 1 ; j < n ; j++)
    {
      reply.val[j] = getcSPI();
    }

  if (r == R1b)
    {
      do { putcSPI (SD_NULL); } // keep sclk moving
      while (getcSPI() == 0x00u);
    }

  SD_CS = 1;
}

// length is determined from command.
void sdcard_proc_block (unsigned char *block)
{
  sdcard_process (R1);
  SD_CS = 0;
  crc.value = 0;
  block[0] = 0;
  for (bidx = 0xff ; bidx && (reply.val[0] == 0u && block[0] != 0xfe) ; bidx--)
    block[0] = getcSPI(); // wait for start bit

  if (block[0] == 0xfeu)
    {
      for (bidx = 0 ; bidx < 0x10u ; bidx++)
        {
          block[bidx] = getcSPI();
          crc.value = sdcard_crc16 (block[bidx], crc.value);
        }
      expected._byte[1] = getcSPI();
      expected._byte[0] = getcSPI();
      getcSPI(); // read the last bit that is required to be 1
      for (bidx = 0xffff ; bidx && sdcard_get_status() ; bidx--)
        putcSPI(SD_NULL);
    }

  sdcard_broadcast_xfer (true, reply.val[0], crc.value == expected.value);
  SD_CS = 1;
}

void sdcard_update_mbr(void)
{
  static unsigned char idx, read_crc, write_crc;

  if (sdcard.total_pages <= sdcard.read_page)  sdcard.read_page = 0;
  if (sdcard.total_pages <= sdcard.write_page) sdcard.write_page = 0;

  mbr.read_page_1  = mbr.read_page_2  = sdcard.read_page;
  mbr.write_page_1 = mbr.write_page_2 = sdcard.write_page;
  read_crc = write_crc = 0;
  for (idx = 0 ; idx < 4u ; idx++) 
    {
      read_crc  = sdcard_crc7 (mbr._byte[6 + idx], read_crc);
      write_crc = sdcard_crc7 (mbr._byte[16 + idx], write_crc);
    }
  mbr.read_crc_1  = mbr.read_crc_2  = read_crc;
  mbr.write_crc_1 = mbr.write_crc_2 = write_crc;
  for (idx = 5 ; idx < sizeof (sd_mbr_t) ; idx++)
    eeprom_write (SD_CIRC_BUFF_INDICES_ADDR + idx, mbr._byte[idx]);
}

void sdcard_complete_block (void)
{
  if (offset & 0x8000) 
    {
      while (offset & 0x1ff) sdcard_write (0x0);
    }

  if (offset & 0x4000)
    {
      while (offset & 0x1ff) sdcard_read();
    }

  offset = 0;
}

void sdcard_erase(void)
{
  sdcard_complete_block();
  sdcard.read_page = sdcard.write_page;
  sdcard_update_mbr();
}

unsigned int sdcard_get_status(void)
{
  sdcard_complete_block();
  cmd = SD_SEND_STATUS; arg = 0x0;
  sdcard_process (R2);
  return reply.r2_val;
}

void sdcard_initialize(void)
{
  static bool_t sdsc;
  unsigned char i;
  static unsigned long int acmd41_arg;

  sdsc = true;
  acmd41_arg = 0x00u;
  version = 0x00u;
  reply.val[0] = 0x80;
  sdcard.read_page   = 0;
  sdcard.total_pages = 0;
  sdcard.write_page  = 0;
  offset = 0;
  SD_CS_INIT();
  Delay1KTCYx (0); // make sure power has been on for 1 ms
  OpenSPI (SPI_FOSC_64, MODE_00, SMPEND); // initial contact must be < 400 KHz

  // CS and MOSI (DI) must be high for 74 sclks to enter native command mode
  SD_CS = 1;
  for (i = 0x0b ; i ; i--) putcSPI (SD_NULL); // keep MOSI high and sclk moving
  sdcard_broadcast (SD_POWER_UP, 0x00u, version);

  // put the sdcard into idle SPI mode
  cmd = SD_GO_IDLE_STATE; arg = 0;
  sdcard_process (R1);
  sdcard_broadcast (SD_SPI_MODE, reply.val[0], reply.val[1]);

  if (reply.val[0] == 0x01u)
    {
      cmd = SD_SEND_IF_COND; arg = 0x000001aa;
      sdcard_process (R7);
      sdcard_broadcast (SD_VOLT_CHK, reply.val[0], version);

      if (reply.r1.illegal_cmd && (reply.val[0] & 0xfa) == 0x00u) version = 1;
      else if (reply.val[0] == 0x01u)
        {
          acmd41_arg = 0x40000000;
          version = 2;
        }
      else return;

      if (version == 2u)
        {
          sdcard_broadcast (SD_VOLT_R00, reply.val[0], version);
          if (reply.val[0] != 0x01u) return;

          sdcard_broadcast (SD_VOLT_R03, reply.val[3], reply.val[2]);
          if ((reply.val[3] & 0x0f) != 0x01u) return;
          
          sdcard_broadcast (SD_VOLT_R04, reply.val[4], version);
          if (reply.val[4] != 0xaau) return;
        }

      OpenSPI (SPI_FOSC_4, MODE_00, SMPMID); // speed up now that we are talking
      Delay10TCYx (12); // give the hardware some time for the clock change
      sdcard_broadcast (SD_CLK_RATE, 0x00u, version);

      // tell the sdcard to initialize
      cmd = SD_APP_CMD; arg = 0x00;
      sdcard_process (R1);
      cmd = SD_APP_SEND_OP_COND; arg = acmd41_arg;
      sdcard_process (R1);
      sdcard_broadcast (SD_INIT_SCD, reply.val[0], version);

      if ((reply.val[0] & 0xfe) != 0x00u) return;

      while (reply.val[0] == 0x01u)
        {
          
          cmd = SD_APP_CMD; arg = 0x00;
          sdcard_process (R1);
          cmd = SD_APP_SEND_OP_COND; arg = acmd41_arg;
          sdcard_process (R1);
        }
      sdcard_broadcast (SD_WAIT_SCD, reply.val[0], version);

      if (reply.val[0] != 0x00u) return;

      if (version == 2u)
        {
          cmd = SD_READ_OCR; arg = 0x00u;
          sdcard_process (R3);
          sdcard_broadcast (SD_OCR_READ, reply.val[0], version);
          sdsc = 0u == (reply.val[1] & 0x40);

          if (reply.val[0] != 0x00u) return;
        }

      // make all cards compatible since SDCH/X cards are fixed at 512 anyway
      cmd = SD_SET_BLOCKLEN; arg = SD_PAGE_SIZE;
      sdcard_process (R1);
      for (i = 0 ; i < sizeof (sdcard.cid) ; i++) sdcard.cid[i] = sdcard.csd[i] = 0x00u;
      cmd = SD_SEND_CID; arg = 0;
      sdcard_proc_block (sdcard.cid);
      sdcard_broadcast (SD_CID_READ, reply.val[0], version);

      if (reply.val[0] != 0x0u && sdcard_get_status() != 0x00u) return;

      cmd = SD_SEND_CSD; arg = 0;
      sdcard_proc_block (sdcard.csd);
      sdcard_broadcast (SD_CSD_READ, reply.val[0], version);

      if (reply.val[0] != 0x0u && sdcard_get_status() != 0x00u) return;

      if (version == 2u)
        sdcard.total_pages = (((unsigned long int)sdcard.csd[9]         |
                              (((unsigned long int)sdcard.csd[8]) << 8) |
                               (((unsigned long int)(sdcard.csd[7] & 0x3f))
                                << 16)) + 1) <<10;
      sdcard_load_mbr();
      sdcard_broadcast (sdsc ? SD_INIT_DONE_SDSC : SD_INIT_DONE_SDSH_X,
                        reply.val[0], version);
    }
}

unsigned char sdcard_read (void)
{
  unsigned char result;

  if (offset & 0x8000) sdcard_complete_block();
  else offset &= 0x1ff;

  if (offset == 0u)
    { // new block
      cmd = SD_READ_BLOCK; arg = sdcard.read_page;
      sdcard_process (R1);
      SD_CS = 0;
      crc.value = 0;
      result = 0;
      for (bidx = 0xff ; bidx &&(reply.val[0] == 0u && result != 0xfe) ; bidx--)
        result = getcSPI(); // wait for start bit
      sdcard_broadcast_xfer (true, reply.val[0], result == 0xfeu);

      if (result == 0xfeu) offset = SD_PAGE_SIZE;
      else offset = 0;
    }
  else result = 0xfeu;

  if (result == 0xfeu)
    {
      SD_CS = 0;
      result = getcSPI();
      crc.value = sdcard_crc16 (result, crc.value);
      offset--;
    }

  if (0u < offset) offset |= 0x4000;
  else
    { // close the block
      expected._byte[1] = getcSPI();
      expected._byte[0] = getcSPI();
      getcSPI(); // read the last bit that is required to be 1
      for (bidx = 0xffff ; bidx && sdcard_get_status() ; bidx--)
        putcSPI(SD_NULL);
      sdcard_broadcast_xfer (true, reply.val[0], crc.value == expected.value);

      if (sdcard_get_status() == 0u)
        {
          sdcard.read_page++;
          sdcard_update_mbr();
        }
    }

  SD_CS = 1;
  return result;
}

void sdcard_write (unsigned char c)
{
  unsigned char result;

  if (offset & 0x4000) sdcard_complete_block();
  else offset &= 0x1ff;

  if (offset == 0u)
    { // new block
      cmd = SD_WRITE_BLOCK;  arg = sdcard.write_page;
      sdcard_process (R1);
      SD_CS = 0;
      crc.value = 0;
      sdcard_broadcast_xfer (false, reply.val[0], true);

      if (reply.val[0] == 0x0u)
        {
          offset = SD_PAGE_SIZE;
          putcSPI (0xfe);
        }
      else
        {
          offset = 0;
          SD_CS = 1;
        }
    }

  if (0u < offset)
    {
      SD_CS = 0;
      putcSPI (c);
      crc.value = sdcard_crc16 (c, crc.value);
      offset--;
    }

  if (0u < offset) offset |= 0x8000;
  else if (SD_CS == 0u)
    { // close the block
      putcSPI (crc._byte[1]);
      putcSPI (crc._byte[0]);
      result = getcSPI();
      sdcard_broadcast_xfer (false, result, (result & 0x0e) == 0x04u);

      if ((result & 0x0e) == 0x04u)
        {
          sdcard.write_page++;
          sdcard_update_mbr();
        }

      while (result != 0xff) result = getcSPI();
      for (bidx = 0xffff ; bidx & sdcard_get_status() ; bidx--)
        putcSPI(SD_NULL);
    }
  SD_CS = 1;
}

/**
******************************************************************************
* @file    hci.c 
* @author  AMG RF Application Team
* @brief   Function for managing framework required for handling HCI interface.
******************************************************************************
*
*
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
* TIME. AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY
* DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
* FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
* CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
*
* <h2><center>&copy; COPYRIGHT 2013 STMicroelectronics</center></h2>
*/ 

#include "hal_types.h"
#include "osal.h"
#include "ble_status.h"
#include "hal.h"
#include "hci_const.h"
#include "gp_timer.h"
#include "bluenrg1_types.h"
#if ENABLE_MICRO_SLEEP /* only STM32L1xx STD library */   
#include "low_power.h"
#endif 


#if DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

#define HCI_READ_PACKET_NUM_MAX 		 (5)

#define MIN(a,b)            ((a) < (b) )? (a) : (b)
#define MAX(a,b)            ((a) > (b) )? (a) : (b)

tListNode hciReadPktPool;
tListNode hciReadPktRxQueue;
/* pool of hci read packets */
static tHciDataPacket     hciReadPacketBuffer[HCI_READ_PACKET_NUM_MAX];

uint8_t BlueNRG_Stack_Initialization(void)
{
  uint8_t index;
  uint8_t ret = 0;
  
  /* Initialize list heads of ready and free hci data packet queues */
  list_init_head (&hciReadPktPool);
  list_init_head (&hciReadPktRxQueue);
  
  /* Initialize the queue of free hci data packets */
  for (index = 0; index < HCI_READ_PACKET_NUM_MAX; index++)
  {
    list_insert_tail(&hciReadPktPool, (tListNode *)&hciReadPacketBuffer[index]);
  }
  
  /* Enable the Select Pin for the communication interface, if supported */
  BlueNRG_Activate_Select_Pin();
  
  /* Reset BlueNRG-1 */
  BlueNRG_RST(); 
  
  return ret;
}

#define HCI_PCK_TYPE_OFFSET                 0
#define  EVENT_PARAMETER_TOT_LEN_OFFSET     2

/**
* Verify if HCI packet is correctly formatted.
*
* @param[in] hciReadPacket    The packet that is received from HCI interface.
* @return 0 if HCI packet is as expected
*/
int HCI_verify(const tHciDataPacket * hciReadPacket)
{
  const uint8_t *hci_pckt = hciReadPacket->dataBuff;
  
  if(hci_pckt[HCI_PCK_TYPE_OFFSET] != HCI_EVENT_PKT)
    return 1;  /* Incorrect type. */
  
  if(hci_pckt[EVENT_PARAMETER_TOT_LEN_OFFSET] != hciReadPacket->data_len - (1+HCI_EVENT_HDR_SIZE))
    return 2; /* Wrong length (packet truncated or too long). */
  
  return 0;      
}


#ifdef BTLE_NWK_COPROC

void BTLE_StackTick(void)
{
  uint32_t i;
  tHciDataPacket * hciReadPacket = NULL;
  
  Disable_IRQ();
  uint8_t list_empty = list_is_empty(&hciReadPktRxQueue);        
  /* process any pending events read */
  while(list_empty == FALSE)
  {
    list_remove_head (&hciReadPktRxQueue, (tListNode **)&hciReadPacket);
    Enable_IRQ(); 
    
    hci_uart_pckt *hci_pckt = (hci_uart_pckt *)hciReadPacket->dataBuff;
    
    if(hci_pckt->type == HCI_EVENT_PKT) {
      hci_event_pckt *event_pckt = (hci_event_pckt*)hci_pckt->data;
      
      if(event_pckt->evt == EVT_LE_META_EVENT) {
        evt_le_meta_event *evt = (void *)event_pckt->data;
        
        for (i = 0; i < (sizeof(hci_le_meta_events_table)/sizeof(hci_le_meta_events_table_type)); i++) {
          if (evt->subevent == hci_le_meta_events_table[i].evt_code) {
            hci_le_meta_events_table[i].process((void *)evt->data);
            break;
          }
        }
      }
      else if(event_pckt->evt == EVT_VENDOR) {
        evt_blue_aci *blue_evt = (void*)event_pckt->data;        
        
        for (i = 0; i < (sizeof(hci_vendor_specific_events_table)/sizeof(hci_vendor_specific_events_table_type)); i++) {
          if (blue_evt->ecode == hci_vendor_specific_events_table[i].evt_code) {
            hci_vendor_specific_events_table[i].process((void *)blue_evt->data);
            break;
          }
        }
      }
      else {
        for (i = 0; i < (sizeof(hci_events_table)/sizeof(hci_events_table_type)); i++) {
          if (event_pckt->evt == hci_events_table[i].evt_code) {
            hci_events_table[i].process((void *)event_pckt->data);
            break;
          }
        }
      }
    }
    
    Disable_IRQ();
    list_insert_tail(&hciReadPktPool, (tListNode *)hciReadPacket);
    list_empty = list_is_empty(&hciReadPktRxQueue);
  }
  
  /* Explicit call to HCI_Isr(), since it cannot be called by ISR if IRQ is kept high by
  BlueNRG. */

  HCI_Isr(); 
  Enable_IRQ();
  
}

#else

volatile uint8_t command_fifo[265];
volatile uint16_t command_fifo_size = 0;

#ifdef DTM_SPI
volatile uint8_t spi_irq_flag = FALSE;
extern uint8_t sdk_buffer[SDK_BUFFER_SIZE];
void BTLE_StackTick(void)
{
  if(spi_irq_flag == TRUE) {
    spi_irq_flag = FALSE;
    //BlueNRG_SPI_Read_Bridge();
    BlueNRG_SPI_Read(sdk_buffer, SDK_BUFFER_SIZE);
  }
  
  if ((command_fifo_size > 0)) {
    /* Run a WRITE request */
//    HAL_NVIC_DisableIRQ(UART_IRQ);
  //__disable_irq();
  //  while(BlueNRG_SPI_Write_Bridge((uint8_t *)command_fifo, command_fifo_size)<0);

    BlueNRG_SPI_Write((uint8_t *)command_fifo, command_fifo_size);
    command_fifo_size = 0;
//    HAL_NVIC_EnableIRQ(UART_IRQ);
 // __enable_irq();
  }
}
#else
#ifdef DTM_UART

#define UART_ARRAY_SIZE 1024
uint8_t DTM_write_data[UART_ARRAY_SIZE];
uint16_t DTM_write_data_head = 0;
uint16_t DTM_write_data_tail = 0;
uint16_t DTM_write_data_size = 0;

uint8_t restart_rx = 0;

void BTLE_StackTick(void)
{
  if ((command_fifo_size > 0)) {
#ifdef DTM_UART_HW_FLOW_CTRL
    DTM_Config_UART_CTS();
    DTM_UART_RTS_OUTPUT_LOW();
#endif
    
    for(uint16_t i=0; i<command_fifo_size; i++) {
      LL_USART_TransmitData8(DTM_USART, command_fifo[i]);
      while(LL_USART_IsActiveFlag_TXE(DTM_USART) == 0);
    }
    while(LL_USART_IsActiveFlag_TC(DTM_USART) == 0);
    
    command_fifo_size = 0;
#ifdef DTM_UART_HW_FLOW_CTRL
    DTM_UART_RTS_OUTPUT_HIGH();
    DTM_UART_CTS_Input();
    
    while(LL_GPIO_IsInputPinSet(DTM_USART_CTS_GPIO_PORT, DTM_USART_CTS_PIN) == 0);
//    for(volatile uint8_t i=0;i<0xFF;i++);
    
    NVIC_EnableIRQ(DTM_USART_EXTI_IRQn);
#endif
  }
  
  if(LL_USART_IsActiveFlag_TXE(USART2)) {
    
    if(DTM_write_data_size != 0) {
      __disable_irq();
      volatile uint16_t temp_head = DTM_write_data_head;
      __enable_irq();
      if(temp_head > DTM_write_data_tail) {
        
        for(uint16_t i=0; i<(temp_head - DTM_write_data_tail); i++) {
          while(LL_USART_IsActiveFlag_TXE(USART2) == 0);
          LL_USART_TransmitData8(USART2, DTM_write_data[DTM_write_data_tail+i]);
        }
        
        __disable_irq();
        DTM_write_data_size -= (temp_head - DTM_write_data_tail);
        __enable_irq();
        DTM_write_data_tail = temp_head;
      }
      else {
        for(uint16_t i=0; i<(UART_ARRAY_SIZE - DTM_write_data_tail); i++) {
          while(LL_USART_IsActiveFlag_TXE(USART2) == 0);
          LL_USART_TransmitData8(USART2, DTM_write_data[DTM_write_data_tail+i]);
        }
        __disable_irq();
        DTM_write_data_size -= (UART_ARRAY_SIZE - DTM_write_data_tail);
        __enable_irq();
        DTM_write_data_tail = 0;
      }
      
    }
  }
  
}
#endif
#endif
#endif

BOOL HCI_Queue_Empty(void)
{
  return list_is_empty(&hciReadPktRxQueue);
}

void HCI_Isr(void)
{
#ifdef SPI_INTERFACE
  tHciDataPacket * hciReadPacket = NULL;
  uint8_t data_len;
  
  LL_EXTI_ClearFlag_0_31(DTM_SPI_IRQ_EXTI_LINE);
  if(LL_GPIO_IsInputPinSet(DTM_SPI_IRQ_PORT, DTM_SPI_IRQ_PIN)){
    if (list_is_empty (&hciReadPktPool) == FALSE){
      
      /* enqueueing a packet for read */
      list_remove_head (&hciReadPktPool, (tListNode **)&hciReadPacket);
      
      data_len = BlueNRG_SPI_Read(hciReadPacket->dataBuff, HCI_READ_PACKET_SIZE);
      if(data_len > 0){
        hciReadPacket->data_len = data_len;
        if(HCI_verify(hciReadPacket) == 0)
          list_insert_tail(&hciReadPktRxQueue, (tListNode *)hciReadPacket);
        else
          list_insert_head(&hciReadPktPool, (tListNode *)hciReadPacket);          
      }
      else {
        // Insert the packet back into the pool.
        list_insert_head(&hciReadPktPool, (tListNode *)hciReadPacket);
      }
    }
  }
#endif
}

void hci_write(const void* data1, const void* data2, uint8_t n_bytes1, uint8_t n_bytes2){
#ifdef  HCI_LOG_ON
  PRINTF("HCI <- ");
  for(int i=0; i < n_bytes1; i++)
    PRINTF("%02X ", *((uint8_t*)data1 + i));
  for(int i=0; i < n_bytes2; i++)
    PRINTF("%02X ", *((uint8_t*)data2 + i));
  PRINTF("\n");    
#endif
  
  Hal_Write_Serial(data1, data2, n_bytes1, n_bytes2);
}

void hci_send_cmd(uint16_t ogf, uint16_t ocf, uint8_t plen, void *param)
{
  hci_command_hdr hc;
  
  hc.opcode = (cmd_opcode_pack(ogf, ocf));
  hc.plen= plen;
  
  uint8_t header[HCI_HDR_SIZE + HCI_COMMAND_HDR_SIZE];
  header[0] = HCI_COMMAND_PKT;
  Osal_MemCpy(header+1, &hc, sizeof(hc));
  
  hci_write(header, param, sizeof(header), plen);
}

static void move_list(tListNode * dest_list, tListNode * src_list)
{
  pListNode tmp_node;
  
  while(!list_is_empty(src_list)){
    list_remove_tail(src_list, &tmp_node);
    list_insert_head(dest_list, tmp_node);
  }
}

/* It ensures that we have at least half of the free buffers in the pool. */
static void free_event_list(void)
{
  tHciDataPacket * pckt;
  
  Disable_IRQ();
  
  while(list_get_size(&hciReadPktPool) < HCI_READ_PACKET_NUM_MAX/2){
    list_remove_head(&hciReadPktRxQueue, (tListNode **)&pckt);    
    list_insert_tail(&hciReadPktPool, (tListNode *)pckt);
    /* Explicit call to HCI_Isr(), since it cannot be called by ISR if IRQ is kept high by
    BlueNRG */
    HCI_Isr();
  }
  
  Enable_IRQ();
}

int hci_send_req(struct hci_request *r, BOOL async)
{
  uint8_t *ptr;
  uint16_t opcode = (cmd_opcode_pack(r->ogf, r->ocf));
  hci_event_pckt *event_pckt;
  hci_uart_pckt *hci_hdr;
  int to = DEFAULT_TIMEOUT;
  struct timer t;
  tHciDataPacket * hciReadPacket = NULL;
  tListNode hciTempQueue;
  
  list_init_head(&hciTempQueue);
  
  free_event_list();
  
//  Disable_IRQ();
  hci_send_cmd(r->ogf, r->ocf, r->clen, r->cparam);
//  Enable_IRQ();
  
  if(async){
    return 0;
  }
  
  /* Minimum timeout is 1. */
  if(to == 0)
    to = 1;
  
  Timer_Set(&t, to);
  
  while(1) {
    evt_cmd_complete *cc;
    evt_cmd_status *cs;
    evt_le_meta_event *me;
    int len;
    
    while(1){
      if(Timer_Expired(&t)){
        goto failed;
      }
      if(!HCI_Queue_Empty()){
        break;
      }
#if ENABLE_MICRO_SLEEP /* only STM32L1xx STD library */
      Enter_Sleep_Mode();
#endif
    }
    
    /* Extract packet from HCI event queue. */
    Disable_IRQ();
    list_remove_head(&hciReadPktRxQueue, (tListNode **)&hciReadPacket);    
    
    hci_hdr = (void *)hciReadPacket->dataBuff;
    
    if(hci_hdr->type == HCI_EVENT_PKT){
      
      event_pckt = (void *) (hci_hdr->data);
      
      ptr = hciReadPacket->dataBuff + (1 + HCI_EVENT_HDR_SIZE);
      len = hciReadPacket->data_len - (1 + HCI_EVENT_HDR_SIZE);
      
      switch (event_pckt->evt) {
        
      case EVT_CMD_STATUS:
        cs = (void *) ptr;
        
        if (cs->opcode != opcode)
          goto failed;
        
        if (r->event != EVT_CMD_STATUS) {
          if (cs->status) {
            goto failed;
          }
          break;
        }
        
        r->rlen = MIN(len, r->rlen);
        Osal_MemCpy(r->rparam, ptr, r->rlen);
        goto done;
        
      case EVT_CMD_COMPLETE:
        cc = (void *) ptr;
        
        if (cc->opcode != opcode)
          goto failed;
        
        ptr += EVT_CMD_COMPLETE_SIZE;
        len -= EVT_CMD_COMPLETE_SIZE;
        
        r->rlen = MIN(len, r->rlen);
        Osal_MemCpy(r->rparam, ptr, r->rlen);
        goto done;
        
      case EVT_LE_META_EVENT:
        me = (void *) ptr;
        
        if (me->subevent != r->event)
          break;
        
        len -= 1;
        r->rlen = MIN(len, r->rlen);
        Osal_MemCpy(r->rparam, me->data, r->rlen);
        goto done;
        
      case EVT_HARDWARE_ERROR:            
        goto failed;
        
      default:      
        break;
      }
    }
    
    /* If there are no more packets to be processed, be sure there is at list one
    packet in the pool to process the expected event.
    If no free packets are available, discard the processed event and insert it
    into the pool. */
    if(list_is_empty(&hciReadPktPool) && list_is_empty(&hciReadPktRxQueue)){
      list_insert_tail(&hciReadPktPool, (tListNode *)hciReadPacket);
      hciReadPacket=NULL;
    }
    else {
      /* Insert the packet in a different queue. These packets will be
      inserted back in the main queue just before exiting from send_req(), so that
      these events can be processed by the application.
      */
      list_insert_tail(&hciTempQueue, (tListNode *)hciReadPacket);
      hciReadPacket=NULL;
    }
    HCI_Isr();
    Enable_IRQ();
    
  }
  
failed:
  if(hciReadPacket!=NULL){
    list_insert_head(&hciReadPktPool, (tListNode *)hciReadPacket);
  }
  move_list(&hciReadPktRxQueue, &hciTempQueue);
  
  Enable_IRQ();
  return -1;
  
done:
  // Insert the packet back into the pool.
  list_insert_head(&hciReadPktPool, (tListNode *)hciReadPacket); 
  move_list(&hciReadPktRxQueue, &hciTempQueue);
  
  Enable_IRQ();
  return 0;
}



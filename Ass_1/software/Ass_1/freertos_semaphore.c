
#include <stdio.h>
#include <unistd.h>

#include "system.h"
#include "sys/alt_irq.h"
#include "io.h"
#include "altera_avalon_pio_regs.h"

unsigned int temp = 10;
void freq_relay(){
	temp = IORD(FREQUENCY_ANALYSER_BASE, 0);
	printf("%f Hz\n", 16000/(double)temp);
	return;
}

int main()
{
  FILE *lcd;
  lcd = fopen(CHARACTER_LCD_NAME, "w");
	alt_irq_register(FREQUENCY_ANALYSER_IRQ, 0, freq_relay);
	while(1){
	  IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, 0x55);
	  usleep(1000000);
	  IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, 0xaa);
	  usleep(1000000);
	  printf("hello?");
	  freq_relay();
		#define ESC 27
		#define CLEAR_LCD_STRING "[2J"
		fprintf(lcd, "%c%s", ESC, CLEAR_LCD_STRING);
		fprintf(lcd, "Hello %f\n", 16000/(double)temp);
	}

  return 0;
}

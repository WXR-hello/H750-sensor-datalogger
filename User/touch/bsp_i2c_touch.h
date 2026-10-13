#ifndef __I2C_TOUCH_H
#define	__I2C_TOUCH_H

#include "stm32h7xx.h"

/*使用软件IIC，把SOFT_IIC宏设置为1，硬件IIC则设置为0
!!使用硬件IIC时非常容易出现错误，不推荐*/
#define SOFT_IIC      1


/* 如果用的是4.3寸/5寸的RGB屏，其触摸芯片可能是GT1151QM，它的I2C地址不同，需要定义这个宏切换一下地址 */
#define LCD_TOUCH_IC_GT1151QM


#ifndef LCD_TOUCH_IC_GT1151QM
#define GTP_ADDRESS                      0x28
#else
#define GTP_ADDRESS                      0x28
/* 4.3寸和5寸电容屏都用 GT1151QM 这个新触摸芯片，
   只能选择这两个屏幕当中的一个，默认选5寸电容屏（定义宏为1选中）*/
  #define TOUCH_GT1151QM_LCD_4_3    0   //使用的是4.3寸电容屏(GT1151QM)
  #define TOUCH_GT1151QM_LCD_5      1   //使用的是5寸电容屏(GT1151QM)
#endif


#define I2CT_FLAG_TIMEOUT         ((uint32_t)0x1000)
#define I2CT_LONG_TIMEOUT         ((uint32_t)(10 * I2CT_FLAG_TIMEOUT))

/*I2C引脚*/
#define GTP_I2C                          I2C2
#define GTP_I2C_CLK_ENABLE()             __HAL_RCC_I2C2_CLK_ENABLE()
#define GTP_I2C_CLK_INIT				 RCC_APB1PeriphClockCmd	

#define GTP_I2C_SCL_PIN                  GPIO_PIN_4                 
#define GTP_I2C_SCL_GPIO_PORT            GPIOH                       
#define GTP_I2C_SCL_GPIO_CLK_ENABLE()    __HAL_RCC_GPIOH_CLK_ENABLE()
#define GTP_I2C_SCL_AF                   GPIO_AF4_I2C2

#define GTP_I2C_SDA_PIN                  GPIO_PIN_5                  
#define GTP_I2C_SDA_GPIO_PORT            GPIOH                     
#define GTP_I2C_SDA_GPIO_CLK_ENABLE()    __HAL_RCC_GPIOH_CLK_ENABLE()
#define GTP_I2C_SDA_AF                   GPIO_AF4_I2C2

/*复位引脚*/
#define GTP_RST_GPIO_PORT                GPIOG
#define GTP_RST_GPIO_CLK_ENABLE()        __HAL_RCC_GPIOG_CLK_ENABLE()
#define GTP_RST_GPIO_PIN                 GPIO_PIN_7
/*中断引脚*/
#define GTP_INT_GPIO_PORT                GPIOG
#define GTP_INT_GPIO_CLK_ENABLE()        __HAL_RCC_GPIOG_CLK_ENABLE()
#define GTP_INT_GPIO_PIN                 GPIO_PIN_3
#define GTP_INT_EXTI_IRQ                 EXTI3_IRQn
/*中断服务函数*/
#define GTP_IRQHandler                   EXTI3_IRQHandler


//软件IIC使用的宏
#define I2C_SCL_1()  HAL_GPIO_WritePin(GTP_I2C_SCL_GPIO_PORT, GTP_I2C_SCL_PIN,GPIO_PIN_SET)		/* SCL = 1 */
#define I2C_SCL_0()  HAL_GPIO_WritePin(GTP_I2C_SCL_GPIO_PORT, GTP_I2C_SCL_PIN,GPIO_PIN_RESET)		/* SCL = 0 */

#define I2C_SDA_1()  HAL_GPIO_WritePin(GTP_I2C_SDA_GPIO_PORT, GTP_I2C_SDA_PIN,GPIO_PIN_SET)		/* SDA = 1 */
#define I2C_SDA_0()  HAL_GPIO_WritePin(GTP_I2C_SDA_GPIO_PORT, GTP_I2C_SDA_PIN,GPIO_PIN_RESET)		/* SDA = 0 */

#define I2C_SDA_READ()  HAL_GPIO_ReadPin(GTP_I2C_SDA_GPIO_PORT, GTP_I2C_SDA_PIN)	/* 读SDA口线状态 */

//函数接口
void I2C_Touch_Init(void);
uint32_t I2C_WriteBytes(uint8_t ClientAddr,uint8_t* pBuffer,  uint8_t NumByteToWrite);
uint32_t I2C_ReadBytes(uint8_t ClientAddr,uint8_t* pBuffer, uint16_t NumByteToRead);
void I2C_ResetChip(void);
void I2C_GTP_IRQDisable(void);
void I2C_GTP_IRQEnable(void);

#endif /* __I2C_TOUCH_H */

/*-----------------------------------------------------------------------*/
/* ffunicode.c —— FatFs R0.16 的字符编码模块（最小实现）                  */
/*                                                                       */
/*  为什么要这个文件：                                                     */
/*   打开 FF_USE_LFN / FF_FS_EXFAT 之后，ff.c 会调用下面三个函数：         */
/*     ff_wtoupper()  文件名比较时的大小写转换（FAT/exFAT 用 ASCII 规则）  */
/*     ff_oem2uni()   ANSI/OEM -> Unicode                                 */
/*     ff_uni2oem()   Unicode   -> ANSI/OEM                               */
/*   官方的 ffunicode.c 带着各代码页的大表（几十 KB Flash）。              */
/*   本工程的文件名全是 ASCII（IMU0001.CSV），所以这里给一个最小实现：     */
/*     ASCII（< 0x80）原样通过；非 ASCII 一律换成 '?'。                    */
/*   效果：卡里带中文名的文件既不会被误匹配，也不会显示乱码；              */
/*         而我们自己读写的那几个文件名完全正常。                          */
/*-----------------------------------------------------------------------*/

#include "ff.h"

#if FF_USE_LFN

/* Unicode 大写转换（只处理 ASCII 的 a~z） */
DWORD ff_wtoupper (DWORD uni)
{
  if ((uni >= (DWORD)'a') && (uni <= (DWORD)'z'))
  {
    uni -= 0x20U;
  }
  return uni;
}

/* OEM(ANSI) -> Unicode：ASCII 原样返回，其余用 '?' */
WCHAR ff_oem2uni (WCHAR oem, WORD cp)
{
  (void)cp;
  return (oem < 0x80U) ? oem : (WCHAR)'?';
}

/* Unicode -> OEM(ANSI)：ASCII 原样返回，其余用 '?' */
WCHAR ff_uni2oem (DWORD uni, WORD cp)
{
  (void)cp;
  return (uni < 0x80U) ? (WCHAR)uni : (WCHAR)'?';
}

#endif /* FF_USE_LFN */

/************************ (C) COPYRIGHT *****END OF FILE************************/

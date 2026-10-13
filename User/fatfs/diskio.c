/*-----------------------------------------------------------------------*/
/* FatFs 底层磁盘 I/O 模块：SD 卡（SDMMC1）                              */
/*                                                                       */
/*  把 FatFs 和 User/sd/bsp_sd.c（SDMMC1 驱动）接起来：                  */
/*    disk_initialize -> SD_Init()          （插卡 + 认卡）              */
/*    disk_read       -> SD_ReadSectors()   （HAL_SD_ReadBlocks 轮询）   */
/*    disk_write      -> SD_WriteSectors()  （HAL_SD_WriteBlocks 轮询）  */
/*-----------------------------------------------------------------------*/

#include "ff.h"			/* FatFs 基本定义 */
#include "diskio.h"		/* FatFs 底层接口声明 */

#include "./sd/bsp_sd.h"

/* 物理驱动器编号：本工程只有 SD 卡一个盘 */
#define DEV_SD		0

/*-----------------------------------------------------------------------*/
/* 驱动器状态                                                            */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (
	BYTE pdrv		/* 物理驱动器号 */
)
{
	if (pdrv != DEV_SD)
	{
		return STA_NOINIT;
	}

	if (SD_IsReady() == 0U)
	{
		return STA_NOINIT | STA_NODISK;		/* 没卡 */
	}

	if (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
	{
		return STA_NOINIT;
	}

	return 0;
}

/*-----------------------------------------------------------------------*/
/* 初始化驱动器（插卡 + 认卡）                                           */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
	BYTE pdrv		/* 物理驱动器号 */
)
{
	if (pdrv != DEV_SD)
	{
		return STA_NOINIT;
	}

	if (SD_Init() != 0U)
	{
		return STA_NOINIT | STA_NODISK;
	}

	return 0;
}

/*-----------------------------------------------------------------------*/
/* 读扇区                                                                */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (
	BYTE pdrv,		/* 物理驱动器号 */
	BYTE *buff,		/* 读出来的数据放这里 */
	LBA_t sector,	/* 起始扇区（512 字节） */
	UINT count		/* 扇区数 */
)
{
	if (pdrv != DEV_SD)
	{
		return RES_PARERR;
	}

	if (SD_IsReady() == 0U)
	{
		return RES_NOTRDY;
	}

	if (SD_ReadSectors(buff, (uint32_t)sector, (uint32_t)count) != HAL_OK)
	{
		return RES_ERROR;
	}

	return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* 写扇区                                                                */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

DRESULT disk_write (
	BYTE pdrv,			/* 物理驱动器号 */
	const BYTE *buff,	/* 要写的数据 */
	LBA_t sector,		/* 起始扇区（512 字节） */
	UINT count			/* 扇区数 */
)
{
	if (pdrv != DEV_SD)
	{
		return RES_PARERR;
	}

	if (SD_IsReady() == 0U)
	{
		return RES_NOTRDY;
	}

	if (SD_WriteSectors((const uint8_t *)buff, (uint32_t)sector, (uint32_t)count) != HAL_OK)
	{
		return RES_ERROR;
	}

	return RES_OK;
}

#endif

/*-----------------------------------------------------------------------*/
/* 其它控制命令                                                          */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (
	BYTE pdrv,		/* 物理驱动器号 */
	BYTE cmd,		/* 控制命令 */
	void *buff		/* 命令参数 */
)
{
	DRESULT res = RES_PARERR;

	if (pdrv != DEV_SD)
	{
		return RES_PARERR;
	}

	switch (cmd)
	{
	case CTRL_SYNC:
		/* 等卡把缓存里的数据真正写完（HAL_SD_WriteBlocks 是阻塞式的，这里只要确认卡在传输态） */
		if (SD_IsReady() != 0U)
		{
			res = RES_OK;
		}
		else
		{
			res = RES_NOTRDY;
		}
		break;

	case GET_SECTOR_COUNT:
		*(LBA_t *)buff = (LBA_t)SD_GetSectorCount();
		res = RES_OK;
		break;

	case GET_SECTOR_SIZE:
		*(WORD *)buff = 512;
		res = RES_OK;
		break;

	case GET_BLOCK_SIZE:
		*(DWORD *)buff = 1;
		res = RES_OK;
		break;

	default:
		res = RES_PARERR;
		break;
	}

	return res;
}

/*-----------------------------------------------------------------------*/
/* 时间戳                                                                */
/*                                                                       */
/*  ffconf.h 里 FF_FS_NORTC = 1，FatFs 不会调用这个函数；               */
/*  以后要是接了 RTC，把 FF_FS_NORTC 改成 0，再在这里返回真实时间即可。  */
/*  返回格式：(年-1980)<<25 | 月<<21 | 日<<16 | 时<<11 | 分<<5 | 秒/2   */
/*-----------------------------------------------------------------------*/

DWORD get_fattime (void)
{
	return ((DWORD)(2025 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}

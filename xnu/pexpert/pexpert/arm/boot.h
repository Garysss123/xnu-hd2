/*
 * Copyright 2013, winocm. <winocm@icloud.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 *   Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 *   Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 * 
 *   If you are going to use this software in any form that does not involve
 *   releasing the source to this project or improving it, let me know beforehand.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _PEXPERT_ARM_BOOT_H_
#define _PEXPERT_ARM_BOOT_H_

#define BOOT_LINE_LENGTH        256

/*
 * Video information.
 */

struct Boot_Video {
	unsigned long	v_baseAddr;
    unsigned long	v_display;
	unsigned long	v_rowBytes;
    unsigned long	v_width;	
	unsigned long	v_height;	
	unsigned long	v_depth;	
};

#define GRAPHICS_MODE         1
#define FB_TEXT_MODE          2

#define kBootVideoDepthMask         (0xFF)
#define kBootVideoDepthDepthShift	(0)
#define kBootVideoDepthRotateShift	(8)
#define kBootVideoDepthScaleShift	(16)

typedef struct Boot_Video	Boot_Video;

#define kBootArgsRevision		1
#define kBootArgsVersion1		1
#define kBootArgsVersion2		2

/*
 * Boot arguments passed by the bootloader.
 */

typedef struct boot_args {
	uint16_t		Revision;
	uint16_t		Version;
	uint32_t		virtBase;
	uint32_t		physBase;
	uint32_t		memSize;
	uint32_t		topOfKernelData;
	Boot_Video		Video;
	uint32_t		machineType;
	void			*deviceTreeP;
	uint32_t		deviceTreeLength;
	char			CommandLine[BOOT_LINE_LENGTH];
} boot_args;

#endif /* _PEXPERT_ARM_BOOT_H_ */

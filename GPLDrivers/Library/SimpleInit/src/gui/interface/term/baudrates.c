/*
 *
 * Copyright (C) 2021 BigfootACA <bigfoot@classfun.cn>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 */

#include"serial.h"
struct baudrate serial_baudrates[]={
	{0000001,50,"50"},
	{0000002,75,"75"},
	{0000003,110,"110"},
	{0000004,134,"134"},
	{0000005,150,"150"},
	{0000006,200,"200"},
	{0000007,300,"300"},
	{0000010,600,"600"},
	{0000011,1200,"1200"},
	{0000012,1800,"1800"},
	{0000013,2400,"2400"},
	{0000014,4800,"4800"},
	{0000015,9600,"9600"},
	{0000016,19200,"19200"},
	{0000017,38400,"38400"},
	{0010001,57600,"57600"},
	{0010002,115200,"115200"},
	{0010003,230400,"230400"},
	{0010004,460800,"460800"},
	{0010005,500000,"500000"},
	{0010006,576000,"576000"},
	{0010007,921600,"921600"},
	{0010010,1000000,"1000000"},
	{0010011,1152000,"1152000"},
	{0010012,1500000,"1500000"},
	{0010013,2000000,"2000000"},
	{0010014,2500000,"2500000"},
	{0010015,3000000,"3000000"},
	{0010016,3500000,"3500000"},
	{0010017,4000000,"4000000"},
	{0,0,""}
};

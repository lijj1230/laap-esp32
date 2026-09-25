#pragma once
#include <Arduino.h>

// ============================================================
//  板载 2 寸屏的电容触摸（FT6236 / FT6336，I2C 0x38）
//  —— 与显示共用 Wire(SDA=1 SCL=2)，由 laap_display.begin() 初始化总线
//  寄存器：0x00 工作模式(0=正常) 0x02 TD_STATUS(低4位=触点数)
//          0x03 P1_XH(bit7-6=事件 0按下/1抬起/2持续) 0x04 P1_XL 0x05 P1_YH 0x06 P1_YL
// ============================================================
#define LTP_ADDR 0x38

bool touchInit();                    // 探活 + 置工作模式；返回 false=无触摸芯片
bool touchPresent();
bool touchRead(int& x, int& y);      // true=当前有触点（含持续按住），xy=面板原始坐标
void touchReadRaw(uint8_t* buf5);    // 诊断：直接吐 0x02 起的 5 个寄存器
uint8_t touchReg(uint8_t reg);       // 诊断：读单寄存器

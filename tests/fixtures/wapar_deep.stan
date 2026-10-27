// Deeply nested expressions, branches, loops and function calls in
// generated quantities: lowering recurses once per level.
functions {
  real f0(real x) { return x + 1; }
  real f1(real x) { return f0(x) + 1; }
  real f2(real x) { return f1(x) + 1; }
  real f3(real x) { return f2(x) + 1; }
  real f4(real x) { return f3(x) + 1; }
  real f5(real x) { return f4(x) + 1; }
  real f6(real x) { return f5(x) + 1; }
  real f7(real x) { return f6(x) + 1; }
  real f8(real x) { return f7(x) + 1; }
  real f9(real x) { return f8(x) + 1; }
  real f10(real x) { return f9(x) + 1; }
  real f11(real x) { return f10(x) + 1; }
  real f12(real x) { return f11(x) + 1; }
  real f13(real x) { return f12(x) + 1; }
  real f14(real x) { return f13(x) + 1; }
  real f15(real x) { return f14(x) + 1; }
  real f16(real x) { return f15(x) + 1; }
  real f17(real x) { return f16(x) + 1; }
  real f18(real x) { return f17(x) + 1; }
  real f19(real x) { return f18(x) + 1; }
  real f20(real x) { return f19(x) + 1; }
  real f21(real x) { return f20(x) + 1; }
  real f22(real x) { return f21(x) + 1; }
  real f23(real x) { return f22(x) + 1; }
  real f24(real x) { return f23(x) + 1; }
  real f25(real x) { return f24(x) + 1; }
  real f26(real x) { return f25(x) + 1; }
  real f27(real x) { return f26(x) + 1; }
  real f28(real x) { return f27(x) + 1; }
  real f29(real x) { return f28(x) + 1; }
  real f30(real x) { return f29(x) + 1; }
  real f31(real x) { return f30(x) + 1; }
  real f32(real x) { return f31(x) + 1; }
  real f33(real x) { return f32(x) + 1; }
  real f34(real x) { return f33(x) + 1; }
  real f35(real x) { return f34(x) + 1; }
  real f36(real x) { return f35(x) + 1; }
  real f37(real x) { return f36(x) + 1; }
  real f38(real x) { return f37(x) + 1; }
  real f39(real x) { return f38(x) + 1; }
  real f40(real x) { return f39(x) + 1; }
  real f41(real x) { return f40(x) + 1; }
  real f42(real x) { return f41(x) + 1; }
  real f43(real x) { return f42(x) + 1; }
  real f44(real x) { return f43(x) + 1; }
  real f45(real x) { return f44(x) + 1; }
  real f46(real x) { return f45(x) + 1; }
  real f47(real x) { return f46(x) + 1; }
  real f48(real x) { return f47(x) + 1; }
  real f49(real x) { return f48(x) + 1; }
  real f50(real x) { return f49(x) + 1; }
  real f51(real x) { return f50(x) + 1; }
  real f52(real x) { return f51(x) + 1; }
  real f53(real x) { return f52(x) + 1; }
  real f54(real x) { return f53(x) + 1; }
  real f55(real x) { return f54(x) + 1; }
  real f56(real x) { return f55(x) + 1; }
  real f57(real x) { return f56(x) + 1; }
  real f58(real x) { return f57(x) + 1; }
  real f59(real x) { return f58(x) + 1; }
  real f60(real x) { return f59(x) + 1; }
  real f61(real x) { return f60(x) + 1; }
  real f62(real x) { return f61(x) + 1; }
  real f63(real x) { return f62(x) + 1; }
  real f64(real x) { return f63(x) + 1; }
  real f65(real x) { return f64(x) + 1; }
  real f66(real x) { return f65(x) + 1; }
  real f67(real x) { return f66(x) + 1; }
  real f68(real x) { return f67(x) + 1; }
  real f69(real x) { return f68(x) + 1; }
  real f70(real x) { return f69(x) + 1; }
  real f71(real x) { return f70(x) + 1; }
  real f72(real x) { return f71(x) + 1; }
  real f73(real x) { return f72(x) + 1; }
  real f74(real x) { return f73(x) + 1; }
  real f75(real x) { return f74(x) + 1; }
  real f76(real x) { return f75(x) + 1; }
  real f77(real x) { return f76(x) + 1; }
  real f78(real x) { return f77(x) + 1; }
  real f79(real x) { return f78(x) + 1; }
  real f80(real x) { return f79(x) + 1; }
  real f81(real x) { return f80(x) + 1; }
  real f82(real x) { return f81(x) + 1; }
  real f83(real x) { return f82(x) + 1; }
  real f84(real x) { return f83(x) + 1; }
  real f85(real x) { return f84(x) + 1; }
  real f86(real x) { return f85(x) + 1; }
  real f87(real x) { return f86(x) + 1; }
  real f88(real x) { return f87(x) + 1; }
  real f89(real x) { return f88(x) + 1; }
  real f90(real x) { return f89(x) + 1; }
  real f91(real x) { return f90(x) + 1; }
  real f92(real x) { return f91(x) + 1; }
  real f93(real x) { return f92(x) + 1; }
  real f94(real x) { return f93(x) + 1; }
  real f95(real x) { return f94(x) + 1; }
  real f96(real x) { return f95(x) + 1; }
  real f97(real x) { return f96(x) + 1; }
  real f98(real x) { return f97(x) + 1; }
  real f99(real x) { return f98(x) + 1; }
  real f100(real x) { return f99(x) + 1; }
  real f101(real x) { return f100(x) + 1; }
  real f102(real x) { return f101(x) + 1; }
  real f103(real x) { return f102(x) + 1; }
  real f104(real x) { return f103(x) + 1; }
  real f105(real x) { return f104(x) + 1; }
  real f106(real x) { return f105(x) + 1; }
  real f107(real x) { return f106(x) + 1; }
  real f108(real x) { return f107(x) + 1; }
  real f109(real x) { return f108(x) + 1; }
  real f110(real x) { return f109(x) + 1; }
  real f111(real x) { return f110(x) + 1; }
  real f112(real x) { return f111(x) + 1; }
  real f113(real x) { return f112(x) + 1; }
  real f114(real x) { return f113(x) + 1; }
  real f115(real x) { return f114(x) + 1; }
  real f116(real x) { return f115(x) + 1; }
  real f117(real x) { return f116(x) + 1; }
  real f118(real x) { return f117(x) + 1; }
  real f119(real x) { return f118(x) + 1; }
  real f120(real x) { return f119(x) + 1; }
  real f121(real x) { return f120(x) + 1; }
  real f122(real x) { return f121(x) + 1; }
  real f123(real x) { return f122(x) + 1; }
  real f124(real x) { return f123(x) + 1; }
  real f125(real x) { return f124(x) + 1; }
  real f126(real x) { return f125(x) + 1; }
  real f127(real x) { return f126(x) + 1; }
  real f128(real x) { return f127(x) + 1; }
  real f129(real x) { return f128(x) + 1; }
  real f130(real x) { return f129(x) + 1; }
  real f131(real x) { return f130(x) + 1; }
  real f132(real x) { return f131(x) + 1; }
  real f133(real x) { return f132(x) + 1; }
  real f134(real x) { return f133(x) + 1; }
  real f135(real x) { return f134(x) + 1; }
  real f136(real x) { return f135(x) + 1; }
  real f137(real x) { return f136(x) + 1; }
  real f138(real x) { return f137(x) + 1; }
  real f139(real x) { return f138(x) + 1; }
  real f140(real x) { return f139(x) + 1; }
  real f141(real x) { return f140(x) + 1; }
  real f142(real x) { return f141(x) + 1; }
  real f143(real x) { return f142(x) + 1; }
  real f144(real x) { return f143(x) + 1; }
  real f145(real x) { return f144(x) + 1; }
  real f146(real x) { return f145(x) + 1; }
  real f147(real x) { return f146(x) + 1; }
  real f148(real x) { return f147(x) + 1; }
  real f149(real x) { return f148(x) + 1; }
  real f150(real x) { return f149(x) + 1; }
  real f151(real x) { return f150(x) + 1; }
  real f152(real x) { return f151(x) + 1; }
  real f153(real x) { return f152(x) + 1; }
  real f154(real x) { return f153(x) + 1; }
  real f155(real x) { return f154(x) + 1; }
  real f156(real x) { return f155(x) + 1; }
  real f157(real x) { return f156(x) + 1; }
  real f158(real x) { return f157(x) + 1; }
  real f159(real x) { return f158(x) + 1; }
  real f160(real x) { return f159(x) + 1; }
  real f161(real x) { return f160(x) + 1; }
  real f162(real x) { return f161(x) + 1; }
  real f163(real x) { return f162(x) + 1; }
  real f164(real x) { return f163(x) + 1; }
  real f165(real x) { return f164(x) + 1; }
  real f166(real x) { return f165(x) + 1; }
  real f167(real x) { return f166(x) + 1; }
  real f168(real x) { return f167(x) + 1; }
  real f169(real x) { return f168(x) + 1; }
  real f170(real x) { return f169(x) + 1; }
  real f171(real x) { return f170(x) + 1; }
  real f172(real x) { return f171(x) + 1; }
  real f173(real x) { return f172(x) + 1; }
  real f174(real x) { return f173(x) + 1; }
  real f175(real x) { return f174(x) + 1; }
  real f176(real x) { return f175(x) + 1; }
  real f177(real x) { return f176(x) + 1; }
  real f178(real x) { return f177(x) + 1; }
  real f179(real x) { return f178(x) + 1; }
  real f180(real x) { return f179(x) + 1; }
  real f181(real x) { return f180(x) + 1; }
  real f182(real x) { return f181(x) + 1; }
  real f183(real x) { return f182(x) + 1; }
  real f184(real x) { return f183(x) + 1; }
  real f185(real x) { return f184(x) + 1; }
  real f186(real x) { return f185(x) + 1; }
  real f187(real x) { return f186(x) + 1; }
  real f188(real x) { return f187(x) + 1; }
  real f189(real x) { return f188(x) + 1; }
  real f190(real x) { return f189(x) + 1; }
  real f191(real x) { return f190(x) + 1; }
  real f192(real x) { return f191(x) + 1; }
  real f193(real x) { return f192(x) + 1; }
  real f194(real x) { return f193(x) + 1; }
  real f195(real x) { return f194(x) + 1; }
  real f196(real x) { return f195(x) + 1; }
  real f197(real x) { return f196(x) + 1; }
  real f198(real x) { return f197(x) + 1; }
  real f199(real x) { return f198(x) + 1; }
  real f200(real x) { return f199(x) + 1; }
  real f201(real x) { return f200(x) + 1; }
  real f202(real x) { return f201(x) + 1; }
  real f203(real x) { return f202(x) + 1; }
  real f204(real x) { return f203(x) + 1; }
  real f205(real x) { return f204(x) + 1; }
  real f206(real x) { return f205(x) + 1; }
  real f207(real x) { return f206(x) + 1; }
  real f208(real x) { return f207(x) + 1; }
  real f209(real x) { return f208(x) + 1; }
  real f210(real x) { return f209(x) + 1; }
  real f211(real x) { return f210(x) + 1; }
  real f212(real x) { return f211(x) + 1; }
  real f213(real x) { return f212(x) + 1; }
  real f214(real x) { return f213(x) + 1; }
  real f215(real x) { return f214(x) + 1; }
  real f216(real x) { return f215(x) + 1; }
  real f217(real x) { return f216(x) + 1; }
  real f218(real x) { return f217(x) + 1; }
  real f219(real x) { return f218(x) + 1; }
  real f220(real x) { return f219(x) + 1; }
  real f221(real x) { return f220(x) + 1; }
  real f222(real x) { return f221(x) + 1; }
  real f223(real x) { return f222(x) + 1; }
  real f224(real x) { return f223(x) + 1; }
  real f225(real x) { return f224(x) + 1; }
  real f226(real x) { return f225(x) + 1; }
  real f227(real x) { return f226(x) + 1; }
  real f228(real x) { return f227(x) + 1; }
  real f229(real x) { return f228(x) + 1; }
  real f230(real x) { return f229(x) + 1; }
  real f231(real x) { return f230(x) + 1; }
  real f232(real x) { return f231(x) + 1; }
  real f233(real x) { return f232(x) + 1; }
  real f234(real x) { return f233(x) + 1; }
  real f235(real x) { return f234(x) + 1; }
  real f236(real x) { return f235(x) + 1; }
  real f237(real x) { return f236(x) + 1; }
  real f238(real x) { return f237(x) + 1; }
  real f239(real x) { return f238(x) + 1; }
  real f240(real x) { return f239(x) + 1; }
  real f241(real x) { return f240(x) + 1; }
  real f242(real x) { return f241(x) + 1; }
  real f243(real x) { return f242(x) + 1; }
  real f244(real x) { return f243(x) + 1; }
  real f245(real x) { return f244(x) + 1; }
  real f246(real x) { return f245(x) + 1; }
  real f247(real x) { return f246(x) + 1; }
  real f248(real x) { return f247(x) + 1; }
  real f249(real x) { return f248(x) + 1; }
}
data {
  int<lower=0, upper=1> flag;
  vector[250] x;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  real total = x[1] + x[2] + x[3] + x[4] + x[5] + x[6] + x[7] + x[8] + x[9] + x[10] + x[11] + x[12] + x[13] + x[14] + x[15] + x[16] + x[17] + x[18] + x[19] + x[20] + x[21] + x[22] + x[23] + x[24] + x[25] + x[26] + x[27] + x[28] + x[29] + x[30] + x[31] + x[32] + x[33] + x[34] + x[35] + x[36] + x[37] + x[38] + x[39] + x[40] + x[41] + x[42] + x[43] + x[44] + x[45] + x[46] + x[47] + x[48] + x[49] + x[50] + x[51] + x[52] + x[53] + x[54] + x[55] + x[56] + x[57] + x[58] + x[59] + x[60] + x[61] + x[62] + x[63] + x[64] + x[65] + x[66] + x[67] + x[68] + x[69] + x[70] + x[71] + x[72] + x[73] + x[74] + x[75] + x[76] + x[77] + x[78] + x[79] + x[80] + x[81] + x[82] + x[83] + x[84] + x[85] + x[86] + x[87] + x[88] + x[89] + x[90] + x[91] + x[92] + x[93] + x[94] + x[95] + x[96] + x[97] + x[98] + x[99] + x[100] + x[101] + x[102] + x[103] + x[104] + x[105] + x[106] + x[107] + x[108] + x[109] + x[110] + x[111] + x[112] + x[113] + x[114] + x[115] + x[116] + x[117] + x[118] + x[119] + x[120] + x[121] + x[122] + x[123] + x[124] + x[125] + x[126] + x[127] + x[128] + x[129] + x[130] + x[131] + x[132] + x[133] + x[134] + x[135] + x[136] + x[137] + x[138] + x[139] + x[140] + x[141] + x[142] + x[143] + x[144] + x[145] + x[146] + x[147] + x[148] + x[149] + x[150] + x[151] + x[152] + x[153] + x[154] + x[155] + x[156] + x[157] + x[158] + x[159] + x[160] + x[161] + x[162] + x[163] + x[164] + x[165] + x[166] + x[167] + x[168] + x[169] + x[170] + x[171] + x[172] + x[173] + x[174] + x[175] + x[176] + x[177] + x[178] + x[179] + x[180] + x[181] + x[182] + x[183] + x[184] + x[185] + x[186] + x[187] + x[188] + x[189] + x[190] + x[191] + x[192] + x[193] + x[194] + x[195] + x[196] + x[197] + x[198] + x[199] + x[200] + x[201] + x[202] + x[203] + x[204] + x[205] + x[206] + x[207] + x[208] + x[209] + x[210] + x[211] + x[212] + x[213] + x[214] + x[215] + x[216] + x[217] + x[218] + x[219] + x[220] + x[221] + x[222] + x[223] + x[224] + x[225] + x[226] + x[227] + x[228] + x[229] + x[230] + x[231] + x[232] + x[233] + x[234] + x[235] + x[236] + x[237] + x[238] + x[239] + x[240] + x[241] + x[242] + x[243] + x[244] + x[245] + x[246] + x[247] + x[248] + x[249] + x[250];
  real branch = 0;
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
if (flag == 1) {
  branch = mu;
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
  real looped = 0;
for (i0 in 1:1) {
for (i1 in 1:1) {
for (i2 in 1:1) {
for (i3 in 1:1) {
for (i4 in 1:1) {
for (i5 in 1:1) {
for (i6 in 1:1) {
for (i7 in 1:1) {
for (i8 in 1:1) {
for (i9 in 1:1) {
for (i10 in 1:1) {
for (i11 in 1:1) {
for (i12 in 1:1) {
for (i13 in 1:1) {
for (i14 in 1:1) {
for (i15 in 1:1) {
for (i16 in 1:1) {
for (i17 in 1:1) {
for (i18 in 1:1) {
for (i19 in 1:1) {
for (i20 in 1:1) {
for (i21 in 1:1) {
for (i22 in 1:1) {
for (i23 in 1:1) {
for (i24 in 1:1) {
for (i25 in 1:1) {
for (i26 in 1:1) {
for (i27 in 1:1) {
for (i28 in 1:1) {
for (i29 in 1:1) {
for (i30 in 1:1) {
for (i31 in 1:1) {
for (i32 in 1:1) {
for (i33 in 1:1) {
for (i34 in 1:1) {
for (i35 in 1:1) {
for (i36 in 1:1) {
for (i37 in 1:1) {
for (i38 in 1:1) {
for (i39 in 1:1) {
for (i40 in 1:1) {
for (i41 in 1:1) {
for (i42 in 1:1) {
for (i43 in 1:1) {
for (i44 in 1:1) {
for (i45 in 1:1) {
for (i46 in 1:1) {
for (i47 in 1:1) {
for (i48 in 1:1) {
for (i49 in 1:1) {
for (i50 in 1:1) {
for (i51 in 1:1) {
for (i52 in 1:1) {
for (i53 in 1:1) {
for (i54 in 1:1) {
for (i55 in 1:1) {
for (i56 in 1:1) {
for (i57 in 1:1) {
for (i58 in 1:1) {
for (i59 in 1:1) {
for (i60 in 1:1) {
for (i61 in 1:1) {
for (i62 in 1:1) {
for (i63 in 1:1) {
for (i64 in 1:1) {
for (i65 in 1:1) {
for (i66 in 1:1) {
for (i67 in 1:1) {
for (i68 in 1:1) {
for (i69 in 1:1) {
for (i70 in 1:1) {
for (i71 in 1:1) {
for (i72 in 1:1) {
for (i73 in 1:1) {
for (i74 in 1:1) {
for (i75 in 1:1) {
for (i76 in 1:1) {
for (i77 in 1:1) {
for (i78 in 1:1) {
for (i79 in 1:1) {
for (i80 in 1:1) {
for (i81 in 1:1) {
for (i82 in 1:1) {
for (i83 in 1:1) {
for (i84 in 1:1) {
for (i85 in 1:1) {
for (i86 in 1:1) {
for (i87 in 1:1) {
for (i88 in 1:1) {
for (i89 in 1:1) {
for (i90 in 1:1) {
for (i91 in 1:1) {
for (i92 in 1:1) {
for (i93 in 1:1) {
for (i94 in 1:1) {
for (i95 in 1:1) {
for (i96 in 1:1) {
for (i97 in 1:1) {
for (i98 in 1:1) {
for (i99 in 1:1) {
for (i100 in 1:1) {
for (i101 in 1:1) {
for (i102 in 1:1) {
for (i103 in 1:1) {
for (i104 in 1:1) {
for (i105 in 1:1) {
for (i106 in 1:1) {
for (i107 in 1:1) {
for (i108 in 1:1) {
for (i109 in 1:1) {
for (i110 in 1:1) {
for (i111 in 1:1) {
for (i112 in 1:1) {
for (i113 in 1:1) {
for (i114 in 1:1) {
for (i115 in 1:1) {
for (i116 in 1:1) {
for (i117 in 1:1) {
for (i118 in 1:1) {
for (i119 in 1:1) {
  looped += mu;
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
}
  real chained = f249(mu);
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"flag": 1, "x": [0.001, 0.002, 0.003, 0.004, 0.005, 0.006, 0.007, 0.008, 0.009, 0.01, 0.011, 0.012, 0.013, 0.014, 0.015, 0.016, 0.017, 0.018, 0.019, 0.02, 0.021, 0.022, 0.023, 0.024, 0.025, 0.026, 0.027, 0.028, 0.029, 0.03, 0.031, 0.032, 0.033, 0.034, 0.035, 0.036, 0.037, 0.038, 0.039, 0.04, 0.041, 0.042, 0.043, 0.044, 0.045, 0.046, 0.047, 0.048, 0.049, 0.05, 0.051, 0.052, 0.053, 0.054, 0.055, 0.056, 0.057, 0.058, 0.059, 0.06, 0.061, 0.062, 0.063, 0.064, 0.065, 0.066, 0.067, 0.068, 0.069, 0.07, 0.071, 0.072, 0.073, 0.074, 0.075, 0.076, 0.077, 0.078, 0.079, 0.08, 0.081, 0.082, 0.083, 0.084, 0.085, 0.086, 0.087, 0.088, 0.089, 0.09, 0.091, 0.092, 0.093, 0.094, 0.095, 0.096, 0.097, 0.098, 0.099, 0.1, 0.101, 0.102, 0.103, 0.104, 0.105, 0.106, 0.107, 0.108, 0.109, 0.11, 0.111, 0.112, 0.113, 0.114, 0.115, 0.116, 0.117, 0.118, 0.119, 0.12, 0.121, 0.122, 0.123, 0.124, 0.125, 0.126, 0.127, 0.128, 0.129, 0.13, 0.131, 0.132, 0.133, 0.134, 0.135, 0.136, 0.137, 0.138, 0.139, 0.14, 0.141, 0.142, 0.143, 0.144, 0.145, 0.146, 0.147, 0.148, 0.149, 0.15, 0.151, 0.152, 0.153, 0.154, 0.155, 0.156, 0.157, 0.158, 0.159, 0.16, 0.161, 0.162, 0.163, 0.164, 0.165, 0.166, 0.167, 0.168, 0.169, 0.17, 0.171, 0.172, 0.173, 0.174, 0.175, 0.176, 0.177, 0.178, 0.179, 0.18, 0.181, 0.182, 0.183, 0.184, 0.185, 0.186, 0.187, 0.188, 0.189, 0.19, 0.191, 0.192, 0.193, 0.194, 0.195, 0.196, 0.197, 0.198, 0.199, 0.2, 0.201, 0.202, 0.203, 0.204, 0.205, 0.206, 0.207, 0.208, 0.209, 0.21, 0.211, 0.212, 0.213, 0.214, 0.215, 0.216, 0.217, 0.218, 0.219, 0.22, 0.221, 0.222, 0.223, 0.224, 0.225, 0.226, 0.227, 0.228, 0.229, 0.23, 0.231, 0.232, 0.233, 0.234, 0.235, 0.236, 0.237, 0.238, 0.239, 0.24, 0.241, 0.242, 0.243, 0.244, 0.245, 0.246, 0.247, 0.248, 0.249, 0.25]}

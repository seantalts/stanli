
/tmp/stanli-allocator-rollout.l6S5ZY/apple-ablation-libs/system-on/libstanli_allocator_benchmark.dylib:	file format mach-o arm64

Disassembly of section __TEXT,__text:

0000000000075210 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_>:
   75210: d103c3ff     	sub	sp, sp, #0xf0
   75214: 6d0823e9     	stp	d9, d8, [sp, #0x80]
   75218: a9096ffc     	stp	x28, x27, [sp, #0x90]
   7521c: a90a67fa     	stp	x26, x25, [sp, #0xa0]
   75220: a90b5ff8     	stp	x24, x23, [sp, #0xb0]
   75224: a90c57f6     	stp	x22, x21, [sp, #0xc0]
   75228: a90d4ff4     	stp	x20, x19, [sp, #0xd0]
   7522c: a90e7bfd     	stp	x29, x30, [sp, #0xe0]
   75230: 910383fd     	add	x29, sp, #0xe0
   75234: a9405c14     	ldp	x20, x23, [x0]
   75238: fd400020     	ldr	d0, [x1]
   7523c: fd400041     	ldr	d1, [x2]
   75240: 6d0483e1     	stp	d1, d0, [sp, #0x48]
   75244: f000dc93     	adrp	x19, 0x1c08000 <domain_curr_field+0x1c07fbe>
   75248: 9106da73     	add	x19, x19, #0x1b6
   7524c: 9000dc88     	adrp	x8, 0x1c05000 <domain_curr_field+0x1c04fbe>
   75250: 91353509     	add	x9, x8, #0xd4d
   75254: f81983b3     	stur	x19, [x29, #-0x68]
   75258: d000dd08     	adrp	x8, 0x1c17000 <domain_curr_field+0x1c16fbe>
   7525c: 913f4108     	add	x8, x8, #0xfd0
   75260: a906a7e8     	stp	x8, x9, [sp, #0x68]
   75264: f90033ff     	str	xzr, [sp, #0x60]
   75268: b4000517     	cbz	x23, 0x75308 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xf8>
   7526c: d2800008     	mov	x8, #0x0                ; =0
   75270: d101a3b5     	sub	x21, x29, #0x68
   75274: 9101c3f6     	add	x22, sp, #0x70
   75278: 910183f8     	add	x24, sp, #0x60
   7527c: 910163f9     	add	x25, sp, #0x58
   75280: 9101a3fa     	add	x26, sp, #0x68
   75284: d503201f     	nop
   75288: d503201f     	nop
   7528c: d503201f     	nop
   75290: d503201f     	nop
   75294: d503201f     	nop
   75298: d503201f     	nop
   7529c: d503201f     	nop
   752a0: fc687a80     	ldr	d0, [x20, x8, lsl #3]
   752a4: fd002fe0     	str	d0, [sp, #0x58]
   752a8: 1e602000     	fcmp	d0, d0
   752ac: 540000c6     	b.vs	0x752c4 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xb4>
   752b0: 91000508     	add	x8, x8, #0x1
   752b4: f90033e8     	str	x8, [sp, #0x60]
   752b8: eb17011f     	cmp	x8, x23
   752bc: 54ffff23     	b.lo	0x752a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x90>
   752c0: 14000011     	b	0x75304 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xf4>
   752c4: a901dbf5     	stp	x21, x22, [sp, #0x18]
   752c8: a902e7f8     	stp	x24, x25, [sp, #0x28]
   752cc: f9001ffa     	str	x26, [sp, #0x38]
   752d0: f90003e0     	str	x0, [sp]
   752d4: 910063e0     	add	x0, sp, #0x18
   752d8: aa0203fc     	mov	x28, x2
   752dc: aa0103fb     	mov	x27, x1
   752e0: 94662538     	bl	0x19fe7c0 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIN5Eigen6MatrixIdLin1ELi1ELi0ELin1ELi1EEEEEvPKcS7_RKT_EUldE_S5_JELPv0ELSC_0EEEvSA_S7_S7_RKT0_S7_DpRKT1_ENKUlvE_clEv>
   752e4: f94003e0     	ldr	x0, [sp]
   752e8: aa1b03e1     	mov	x1, x27
   752ec: aa1c03e2     	mov	x2, x28
   752f0: f94033e8     	ldr	x8, [sp, #0x60]
   752f4: 91000508     	add	x8, x8, #0x1
   752f8: f90033e8     	str	x8, [sp, #0x60]
   752fc: eb17011f     	cmp	x8, x23
   75300: 54fffd03     	b.lo	0x752a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x90>
   75304: fd402be0     	ldr	d0, [sp, #0x50]
   75308: f81983b3     	stur	x19, [x29, #-0x68]
   7530c: f000dd08     	adrp	x8, 0x1c18000 <domain_curr_field+0x1c17fbe>
   75310: 910e2908     	add	x8, x8, #0x38a
   75314: 9000dc89     	adrp	x9, 0x1c05000 <domain_curr_field+0x1c04fbe>
   75318: 91362929     	add	x9, x9, #0xd8a
   7531c: a906a3e9     	stp	x9, x8, [sp, #0x68]
   75320: 9e660008     	fmov	x8, d0
   75324: 9240f908     	and	x8, x8, #0x7fffffffffffffff
   75328: d2effe09     	mov	x9, #0x7ff0000000000000 ; =9218868437227405312
   7532c: eb09011f     	cmp	x8, x9
   75330: 5400282a     	b.ge	0x75834 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x624>
   75334: f81983b3     	stur	x19, [x29, #-0x68]
   75338: 9000dc88     	adrp	x8, 0x1c05000 <domain_curr_field+0x1c04fbe>
   7533c: 9135e909     	add	x9, x8, #0xd7a
   75340: 9000dc88     	adrp	x8, 0x1c05000 <domain_curr_field+0x1c04fbe>
   75344: 911c5d08     	add	x8, x8, #0x717
   75348: a906a7e8     	stp	x8, x9, [sp, #0x68]
   7534c: fd4027e0     	ldr	d0, [sp, #0x48]
   75350: 1e602008     	fcmp	d0, #0.0
   75354: 540028ed     	b.le	0x75870 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x660>
   75358: f9400416     	ldr	x22, [x0, #0x8]
   7535c: b4002a96     	cbz	x22, 0x758ac <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x69c>
   75360: f9400028     	ldr	x8, [x1]
   75364: f9400049     	ldr	x9, [x2]
   75368: 910063ea     	add	x10, sp, #0x18
   7536c: a901abff     	stp	xzr, x10, [sp, #0x18]
   75370: 9100614a     	add	x10, x10, #0x18
   75374: a902ffe8     	stp	x8, xzr, [sp, #0x28]
   75378: a903a7ea     	stp	x10, x9, [sp, #0x38]
   7537c: fd4027e8     	ldr	d8, [sp, #0x48]
   75380: b4000a97     	cbz	x23, 0x754d0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x2c0>
   75384: d37cfee8     	lsr	x8, x23, #60
   75388: b5002a88     	cbnz	x8, 0x758d8 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6c8>
   7538c: fd402be0     	ldr	d0, [sp, #0x50]
   75390: 3d8003e0     	str	q0, [sp]
   75394: d37df2f5     	lsl	x21, x23, #3
   75398: aa1503e0     	mov	x0, x21
   7539c: 946741cd     	bl	0x1a45ad0 <domain_curr_field+0x1a45a8e>
   753a0: b40029e0     	cbz	x0, 0x758dc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6cc>
   753a4: aa0003f3     	mov	x19, x0
   753a8: 1e6e1000     	fmov	d0, #1.00000000
   753ac: 927feaf8     	and	x24, x23, #0xffffffffffffffe
   753b0: 1e681802     	fdiv	d2, d0, d8
   753b4: f10006ff     	cmp	x23, #0x1
   753b8: 3dc003e3     	ldr	q3, [sp]
   753bc: 54000200     	b.eq	0x753fc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x1ec>
   753c0: d2800008     	mov	x8, #0x0                ; =0
   753c4: 4e080460     	dup.2d	v0, v3[0]
   753c8: aa1303e9     	mov	x9, x19
   753cc: aa1403ea     	mov	x10, x20
   753d0: d503201f     	nop
   753d4: d503201f     	nop
   753d8: d503201f     	nop
   753dc: d503201f     	nop
   753e0: 3cc10541     	ldr	q1, [x10], #0x10
   753e4: 4ee0d421     	fsub.2d	v1, v1, v0
   753e8: 4fc29021     	fmul.2d	v1, v1, v2[0]
   753ec: 3c810521     	str	q1, [x9], #0x10
   753f0: 91000908     	add	x8, x8, #0x2
   753f4: eb18011f     	cmp	x8, x24
   753f8: 54ffff43     	b.lo	0x753e0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x1d0>
   753fc: eb1802e8     	subs	x8, x23, x24
   75400: 540001c0     	b.eq	0x75438 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x228>
   75404: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   75408: 8b0a0289     	add	x9, x20, x10
   7540c: 8b0a026a     	add	x10, x19, x10
   75410: d503201f     	nop
   75414: d503201f     	nop
   75418: d503201f     	nop
   7541c: d503201f     	nop
   75420: fc408520     	ldr	d0, [x9], #0x8
   75424: 1e633800     	fsub	d0, d0, d3
   75428: 1e600840     	fmul	d0, d2, d0
   7542c: fc008540     	str	d0, [x10], #0x8
   75430: f1000508     	subs	x8, x8, #0x1
   75434: 54ffff61     	b.ne	0x75420 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x210>
   75438: 3d8003e2     	str	q2, [sp]
   7543c: aa1503e0     	mov	x0, x21
   75440: 946741a4     	bl	0x1a45ad0 <domain_curr_field+0x1a45a8e>
   75444: b40024e0     	cbz	x0, 0x758e0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6d0>
   75448: aa0003f4     	mov	x20, x0
   7544c: f10006ff     	cmp	x23, #0x1
   75450: 54000140     	b.eq	0x75478 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x268>
   75454: d2800008     	mov	x8, #0x0                ; =0
   75458: aa1403e9     	mov	x9, x20
   7545c: aa1303ea     	mov	x10, x19
   75460: 3cc10540     	ldr	q0, [x10], #0x10
   75464: 6e60dc00     	fmul.2d	v0, v0, v0
   75468: 3c810520     	str	q0, [x9], #0x10
   7546c: 91000908     	add	x8, x8, #0x2
   75470: eb18011f     	cmp	x8, x24
   75474: 54ffff63     	b.lo	0x75460 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x250>
   75478: eb1802e8     	subs	x8, x23, x24
   7547c: 540001c0     	b.eq	0x754b4 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x2a4>
   75480: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   75484: 8b0a0269     	add	x9, x19, x10
   75488: 8b0a028a     	add	x10, x20, x10
   7548c: d503201f     	nop
   75490: d503201f     	nop
   75494: d503201f     	nop
   75498: d503201f     	nop
   7549c: d503201f     	nop
   754a0: fc408520     	ldr	d0, [x9], #0x8
   754a4: 1e600800     	fmul	d0, d0, d0
   754a8: fc008540     	str	d0, [x10], #0x8
   754ac: f1000508     	subs	x8, x8, #0x1
   754b0: 54ffff81     	b.ne	0x754a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x290>
   754b4: f10006df     	cmp	x22, #0x1
   754b8: 9a9fc6da     	csinc	x26, x22, xzr, gt
   754bc: 927ee6f9     	and	x25, x23, #0xffffffffffffffc
   754c0: f10006ff     	cmp	x23, #0x1
   754c4: 540002a1     	b.ne	0x75518 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x308>
   754c8: fd400289     	ldr	d9, [x20]
   754cc: 14000039     	b	0x755b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3a0>
   754d0: 4ea81d00     	mov.16b	v0, v8
   754d4: 94674167     	bl	0x1a45a70 <domain_curr_field+0x1a45a2e>
   754d8: d2800013     	mov	x19, #0x0               ; =0
   754dc: d2800014     	mov	x20, #0x0               ; =0
   754e0: f10006df     	cmp	x22, #0x1
   754e4: 9a9fc6c8     	csinc	x8, x22, xzr, gt
   754e8: 9e630101     	ucvtf	d1, x8
   754ec: 1e608828     	fnmul	d8, d1, d0
   754f0: f9001bff     	str	xzr, [sp, #0x30]
   754f4: f9000fff     	str	xzr, [sp, #0x18]
   754f8: 6f00e409     	movi.2d	v9, #0000000000000000
   754fc: f000f5c0     	adrp	x0, 0x1f30000 <_pool_freelist+0x1a8>
   75500: 910a6000     	add	x0, x0, #0x298
   75504: f9400008     	ldr	x8, [x0]
   75508: d63f0100     	blr	x8
   7550c: f9400008     	ldr	x8, [x0]
   75510: b5001648     	cbnz	x8, 0x757d8 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5c8>
   75514: 140000c1     	b	0x75818 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75518: 3dc00280     	ldr	q0, [x20]
   7551c: f10012ff     	cmp	x23, #0x4
   75520: 540002a3     	b.lo	0x75574 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x364>
   75524: 3dc00681     	ldr	q1, [x20, #0x10]
   75528: f10022ff     	cmp	x23, #0x8
   7552c: 54000183     	b.lo	0x7555c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x34c>
   75530: 9100c288     	add	x8, x20, #0x30
   75534: 52800089     	mov	w9, #0x4                ; =4
   75538: d503201f     	nop
   7553c: d503201f     	nop
   75540: ad7f8d02     	ldp	q2, q3, [x8, #-0x10]
   75544: 4e62d400     	fadd.2d	v0, v0, v2
   75548: 4e63d421     	fadd.2d	v1, v1, v3
   7554c: 91001129     	add	x9, x9, #0x4
   75550: 91008108     	add	x8, x8, #0x20
   75554: eb19013f     	cmp	x9, x25
   75558: 54ffff43     	b.lo	0x75540 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x330>
   7555c: 4e60d420     	fadd.2d	v0, v1, v0
   75560: eb19031f     	cmp	x24, x25
   75564: 54000089     	b.ls	0x75574 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x364>
   75568: d37df328     	lsl	x8, x25, #3
   7556c: 3ce86a81     	ldr	q1, [x20, x8]
   75570: 4e61d400     	fadd.2d	v0, v0, v1
   75574: 7e70d809     	faddp.2d	d9, v0
   75578: eb1802e8     	subs	x8, x23, x24
   7557c: 540001a0     	b.eq	0x755b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3a0>
   75580: d341eee9     	ubfx	x9, x23, #1, #59
   75584: 8b091289     	add	x9, x20, x9, lsl #4
   75588: d503201f     	nop
   7558c: d503201f     	nop
   75590: d503201f     	nop
   75594: d503201f     	nop
   75598: d503201f     	nop
   7559c: d503201f     	nop
   755a0: fc408520     	ldr	d0, [x9], #0x8
   755a4: 1e602929     	fadd	d9, d9, d0
   755a8: f1000508     	subs	x8, x8, #0x1
   755ac: 54ffffa1     	b.ne	0x755a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x390>
   755b0: aa1503e0     	mov	x0, x21
   755b4: 94674147     	bl	0x1a45ad0 <domain_curr_field+0x1a45a8e>
   755b8: b4001a60     	cbz	x0, 0x75904 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6f4>
   755bc: aa0003f6     	mov	x22, x0
   755c0: 4ea81d00     	mov.16b	v0, v8
   755c4: 9467412b     	bl	0x1a45a70 <domain_curr_field+0x1a45a2e>
   755c8: f10006ff     	cmp	x23, #0x1
   755cc: 3dc003e7     	ldr	q7, [sp]
   755d0: 54000140     	b.eq	0x755f8 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3e8>
   755d4: d2800008     	mov	x8, #0x0                ; =0
   755d8: aa1603e9     	mov	x9, x22
   755dc: aa1303ea     	mov	x10, x19
   755e0: 3cc10541     	ldr	q1, [x10], #0x10
   755e4: 4fc79021     	fmul.2d	v1, v1, v7[0]
   755e8: 3c810521     	str	q1, [x9], #0x10
   755ec: 91000908     	add	x8, x8, #0x2
   755f0: eb18011f     	cmp	x8, x24
   755f4: 54ffff63     	b.lo	0x755e0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3d0>
   755f8: 1e7c1001     	fmov	d1, #-0.50000000
   755fc: 9e630342     	ucvtf	d2, x26
   75600: eb1802e8     	subs	x8, x23, x24
   75604: 54000180     	b.eq	0x75634 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x424>
   75608: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   7560c: 8b0a0269     	add	x9, x19, x10
   75610: 8b0a02ca     	add	x10, x22, x10
   75614: d503201f     	nop
   75618: d503201f     	nop
   7561c: d503201f     	nop
   75620: fc408523     	ldr	d3, [x9], #0x8
   75624: 1e6308e3     	fmul	d3, d7, d3
   75628: fc008543     	str	d3, [x10], #0x8
   7562c: f1000508     	subs	x8, x8, #0x1
   75630: 54ffff81     	b.ne	0x75620 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x410>
   75634: 1e610921     	fmul	d1, d9, d1
   75638: 1e620800     	fmul	d0, d0, d2
   7563c: f10006ff     	cmp	x23, #0x1
   75640: 540000e1     	b.ne	0x7565c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x44c>
   75644: fd400282     	ldr	d2, [x20]
   75648: 1e6208e2     	fmul	d2, d7, d2
   7564c: 1e673842     	fsub	d2, d2, d7
   75650: fd001be2     	str	d2, [sp, #0x30]
   75654: fd4002c9     	ldr	d9, [x22]
   75658: 14000056     	b	0x757b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5a0>
   7565c: 4e0804e3     	dup.2d	v3, v7[0]
   75660: 3dc00282     	ldr	q2, [x20]
   75664: 4fc79042     	fmul.2d	v2, v2, v7[0]
   75668: 4ee3d442     	fsub.2d	v2, v2, v3
   7566c: f10012ff     	cmp	x23, #0x4
   75670: 540003e3     	b.lo	0x756ec <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4dc>
   75674: 3dc00684     	ldr	q4, [x20, #0x10]
   75678: 4fc79084     	fmul.2d	v4, v4, v7[0]
   7567c: 4ee3d484     	fsub.2d	v4, v4, v3
   75680: f10022ff     	cmp	x23, #0x8
   75684: 54000243     	b.lo	0x756cc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4bc>
   75688: 9100c288     	add	x8, x20, #0x30
   7568c: 52800089     	mov	w9, #0x4                ; =4
   75690: d503201f     	nop
   75694: d503201f     	nop
   75698: d503201f     	nop
   7569c: d503201f     	nop
   756a0: ad7f9905     	ldp	q5, q6, [x8, #-0x10]
   756a4: 4fc790a5     	fmul.2d	v5, v5, v7[0]
   756a8: 4ee3d4a5     	fsub.2d	v5, v5, v3
   756ac: 4e65d442     	fadd.2d	v2, v2, v5
   756b0: 4fc790c5     	fmul.2d	v5, v6, v7[0]
   756b4: 4ee3d4a5     	fsub.2d	v5, v5, v3
   756b8: 4e65d484     	fadd.2d	v4, v4, v5
   756bc: 91001129     	add	x9, x9, #0x4
   756c0: 91008108     	add	x8, x8, #0x20
   756c4: eb19013f     	cmp	x9, x25
   756c8: 54fffec3     	b.lo	0x756a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x490>
   756cc: 4e62d482     	fadd.2d	v2, v4, v2
   756d0: eb19031f     	cmp	x24, x25
   756d4: 540000c9     	b.ls	0x756ec <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4dc>
   756d8: d37df328     	lsl	x8, x25, #3
   756dc: 3ce86a84     	ldr	q4, [x20, x8]
   756e0: 4fc79084     	fmul.2d	v4, v4, v7[0]
   756e4: 4ee3d483     	fsub.2d	v3, v4, v3
   756e8: 4e63d442     	fadd.2d	v2, v2, v3
   756ec: 7e70d842     	faddp.2d	d2, v2
   756f0: eb1802e8     	subs	x8, x23, x24
   756f4: 54000120     	b.eq	0x75718 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x508>
   756f8: d341eee9     	ubfx	x9, x23, #1, #59
   756fc: 8b091289     	add	x9, x20, x9, lsl #4
   75700: fc408523     	ldr	d3, [x9], #0x8
   75704: 1e6308e3     	fmul	d3, d7, d3
   75708: 1e673863     	fsub	d3, d3, d7
   7570c: 1e632842     	fadd	d2, d2, d3
   75710: f1000508     	subs	x8, x8, #0x1
   75714: 54ffff61     	b.ne	0x75700 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4f0>
   75718: fd001be2     	str	d2, [sp, #0x30]
   7571c: 3dc002c2     	ldr	q2, [x22]
   75720: f10012ff     	cmp	x23, #0x4
   75724: 54000283     	b.lo	0x75774 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x564>
   75728: 3dc006c3     	ldr	q3, [x22, #0x10]
   7572c: f10022ff     	cmp	x23, #0x8
   75730: 54000163     	b.lo	0x7575c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x54c>
   75734: 9100c2c8     	add	x8, x22, #0x30
   75738: 52800089     	mov	w9, #0x4                ; =4
   7573c: d503201f     	nop
   75740: ad7f9504     	ldp	q4, q5, [x8, #-0x10]
   75744: 4e64d442     	fadd.2d	v2, v2, v4
   75748: 4e65d463     	fadd.2d	v3, v3, v5
   7574c: 91001129     	add	x9, x9, #0x4
   75750: 91008108     	add	x8, x8, #0x20
   75754: eb19013f     	cmp	x9, x25
   75758: 54ffff43     	b.lo	0x75740 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x530>
   7575c: 4e62d462     	fadd.2d	v2, v3, v2
   75760: eb19031f     	cmp	x24, x25
   75764: 54000089     	b.ls	0x75774 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x564>
   75768: d37df328     	lsl	x8, x25, #3
   7576c: 3ce86ac3     	ldr	q3, [x22, x8]
   75770: 4e63d442     	fadd.2d	v2, v2, v3
   75774: 7e70d849     	faddp.2d	d9, v2
   75778: eb1802e8     	subs	x8, x23, x24
   7577c: 540001a0     	b.eq	0x757b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5a0>
   75780: d341eee9     	ubfx	x9, x23, #1, #59
   75784: 8b0912c9     	add	x9, x22, x9, lsl #4
   75788: d503201f     	nop
   7578c: d503201f     	nop
   75790: d503201f     	nop
   75794: d503201f     	nop
   75798: d503201f     	nop
   7579c: d503201f     	nop
   757a0: fc408522     	ldr	d2, [x9], #0x8
   757a4: 1e622929     	fadd	d9, d9, d2
   757a8: f1000508     	subs	x8, x8, #0x1
   757ac: 54ffffa1     	b.ne	0x757a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x590>
   757b0: 1e603828     	fsub	d8, d1, d0
   757b4: fd000fe9     	str	d9, [sp, #0x18]
   757b8: aa1603e0     	mov	x0, x22
   757bc: 94674002     	bl	0x1a457c4 <domain_curr_field+0x1a45782>
   757c0: f000f5c0     	adrp	x0, 0x1f30000 <_pool_freelist+0x1a8>
   757c4: 910a6000     	add	x0, x0, #0x298
   757c8: f9400008     	ldr	x8, [x0]
   757cc: d63f0100     	blr	x8
   757d0: f9400008     	ldr	x8, [x0]
   757d4: b4000228     	cbz	x8, 0x75818 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   757d8: 52800029     	mov	w9, #0x1                ; =1
   757dc: 39022109     	strb	w9, [x8, #0x88]
   757e0: fd004108     	str	d8, [x8, #0x80]
   757e4: f9400509     	ldr	x9, [x8, #0x8]
   757e8: b40000a9     	cbz	x9, 0x757fc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5ec>
   757ec: f940250a     	ldr	x10, [x8, #0x48]
   757f0: f100055f     	cmp	x10, #0x1
   757f4: 54000041     	b.ne	0x757fc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5ec>
   757f8: fd000129     	str	d9, [x9]
   757fc: f9400909     	ldr	x9, [x8, #0x10]
   75800: b40000c9     	cbz	x9, 0x75818 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75804: f9402908     	ldr	x8, [x8, #0x50]
   75808: f100051f     	cmp	x8, #0x1
   7580c: 54000061     	b.ne	0x75818 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75810: fd401be0     	ldr	d0, [sp, #0x30]
   75814: fd000120     	str	d0, [x9]
   75818: b4000074     	cbz	x20, 0x75824 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x614>
   7581c: aa1403e0     	mov	x0, x20
   75820: 94673fe9     	bl	0x1a457c4 <domain_curr_field+0x1a45782>
   75824: b4000473     	cbz	x19, 0x758b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6a0>
   75828: aa1303e0     	mov	x0, x19
   7582c: 94673fe6     	bl	0x1a457c4 <domain_curr_field+0x1a45782>
   75830: 14000020     	b	0x758b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6a0>
   75834: d101a3a8     	sub	x8, x29, #0x68
   75838: 9101c3e9     	add	x9, sp, #0x70
   7583c: a901a7e8     	stp	x8, x9, [sp, #0x18]
   75840: 910143e8     	add	x8, sp, #0x50
   75844: 9101a3e9     	add	x9, sp, #0x68
   75848: a902a7e8     	stp	x8, x9, [sp, #0x28]
   7584c: aa0003f5     	mov	x21, x0
   75850: 910063e0     	add	x0, sp, #0x18
   75854: aa0203f6     	mov	x22, x2
   75858: aa0103f8     	mov	x24, x1
   7585c: 9466b325     	bl	0x1a224f0 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIdEEvPKcS4_RKT_EUldE_dJELPv0EEEvS7_S4_S4_RKT0_S4_DpRKT1_ENKUlvE_clEv>
   75860: aa1503e0     	mov	x0, x21
   75864: aa1803e1     	mov	x1, x24
   75868: aa1603e2     	mov	x2, x22
   7586c: 17fffeb2     	b	0x75334 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x124>
   75870: d101a3a8     	sub	x8, x29, #0x68
   75874: 9101c3e9     	add	x9, sp, #0x70
   75878: a901a7e8     	stp	x8, x9, [sp, #0x18]
   7587c: 910123e8     	add	x8, sp, #0x48
   75880: 9101a3e9     	add	x9, sp, #0x68
   75884: a902a7e8     	stp	x8, x9, [sp, #0x28]
   75888: aa0003f3     	mov	x19, x0
   7588c: 910063e0     	add	x0, sp, #0x18
   75890: aa0203f5     	mov	x21, x2
   75894: aa0103f6     	mov	x22, x1
   75898: 9466b316     	bl	0x1a224f0 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIdEEvPKcS4_RKT_EUldE_dJELPv0EEEvS7_S4_S4_RKT0_S4_DpRKT1_ENKUlvE_clEv>
   7589c: aa1603e1     	mov	x1, x22
   758a0: aa1503e2     	mov	x2, x21
   758a4: f9400676     	ldr	x22, [x19, #0x8]
   758a8: b5ffd5d6     	cbnz	x22, 0x75360 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x150>
   758ac: 6f00e408     	movi.2d	v8, #0000000000000000
   758b0: 4ea81d00     	mov.16b	v0, v8
   758b4: a94e7bfd     	ldp	x29, x30, [sp, #0xe0]
   758b8: a94d4ff4     	ldp	x20, x19, [sp, #0xd0]
   758bc: a94c57f6     	ldp	x22, x21, [sp, #0xc0]
   758c0: a94b5ff8     	ldp	x24, x23, [sp, #0xb0]
   758c4: a94a67fa     	ldp	x26, x25, [sp, #0xa0]
   758c8: a9496ffc     	ldp	x28, x27, [sp, #0x90]
   758cc: 6d4823e9     	ldp	d9, d8, [sp, #0x80]
   758d0: 9103c3ff     	add	sp, sp, #0xf0
   758d4: d65f03c0     	ret
   758d8: 9466129e     	bl	0x19fa350 <__ZZN6stanli9MirInterpIdE8eval_funERKNS_3mir4ExprEENKUlRKNS_7DataMap5EntryEbE_clES9_b.cold.2>
   758dc: 9466129d     	bl	0x19fa350 <__ZZN6stanli9MirInterpIdE8eval_funERKNS_3mir4ExprEENKUlRKNS_7DataMap5EntryEbE_clES9_b.cold.2>
   758e0: 52800100     	mov	w0, #0x8                ; =8
   758e4: 94673ecb     	bl	0x1a45410 <domain_curr_field+0x1a453ce>
   758e8: 94673eac     	bl	0x1a45398 <domain_curr_field+0x1a45356>
   758ec: f000e1c1     	adrp	x1, 0x1cb0000 <domain_curr_field+0x1caffbe>
   758f0: f9407421     	ldr	x1, [x1, #0xe8]
   758f4: f000e1c2     	adrp	x2, 0x1cb0000 <domain_curr_field+0x1caffbe>
   758f8: f9405842     	ldr	x2, [x2, #0xb0]
   758fc: 94673ee0     	bl	0x1a4547c <domain_curr_field+0x1a4543a>
   75900: 14000009     	b	0x75924 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x714>
   75904: 52800100     	mov	w0, #0x8                ; =8
   75908: 94673ec2     	bl	0x1a45410 <domain_curr_field+0x1a453ce>
   7590c: 94673ea3     	bl	0x1a45398 <domain_curr_field+0x1a45356>
   75910: f000e1c1     	adrp	x1, 0x1cb0000 <domain_curr_field+0x1caffbe>
   75914: f9407421     	ldr	x1, [x1, #0xe8]
   75918: f000e1c2     	adrp	x2, 0x1cb0000 <domain_curr_field+0x1caffbe>
   7591c: f9405842     	ldr	x2, [x2, #0xb0]
   75920: 94673ed7     	bl	0x1a4547c <domain_curr_field+0x1a4543a>
   75924: d4200020     	brk	#0x1
   75928: aa0003f5     	mov	x21, x0
   7592c: aa1403e0     	mov	x0, x20
   75930: 94673fa5     	bl	0x1a457c4 <domain_curr_field+0x1a45782>
   75934: aa1303e0     	mov	x0, x19
   75938: 94673fa3     	bl	0x1a457c4 <domain_curr_field+0x1a45782>
   7593c: aa1503e0     	mov	x0, x21
   75940: 94673d52     	bl	0x1a44e88 <domain_curr_field+0x1a44e46>
   75944: aa0003f5     	mov	x21, x0
   75948: aa1303e0     	mov	x0, x19
   7594c: 94673f9e     	bl	0x1a457c4 <domain_curr_field+0x1a45782>
   75950: aa1503e0     	mov	x0, x21
   75954: 94673d4d     	bl	0x1a44e88 <domain_curr_field+0x1a44e46>

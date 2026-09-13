
/tmp/stanli-allocator-rollout.l6S5ZY/apple-ablation-libs/fallback-on/libstanli_allocator_benchmark.dylib:	file format mach-o arm64

Disassembly of section __TEXT,__text:

0000000000075330 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_>:
   75330: d103c3ff     	sub	sp, sp, #0xf0
   75334: 6d0823e9     	stp	d9, d8, [sp, #0x80]
   75338: a9096ffc     	stp	x28, x27, [sp, #0x90]
   7533c: a90a67fa     	stp	x26, x25, [sp, #0xa0]
   75340: a90b5ff8     	stp	x24, x23, [sp, #0xb0]
   75344: a90c57f6     	stp	x22, x21, [sp, #0xc0]
   75348: a90d4ff4     	stp	x20, x19, [sp, #0xd0]
   7534c: a90e7bfd     	stp	x29, x30, [sp, #0xe0]
   75350: 910383fd     	add	x29, sp, #0xe0
   75354: a9405c14     	ldp	x20, x23, [x0]
   75358: fd400020     	ldr	d0, [x1]
   7535c: fd400041     	ldr	d1, [x2]
   75360: 6d0483e1     	stp	d1, d0, [sp, #0x48]
   75364: b000ddb3     	adrp	x19, 0x1c2a000 <domain_curr_field+0x1c29fbe>
   75368: 91313673     	add	x19, x19, #0xc4d
   7536c: f000dd88     	adrp	x8, 0x1c28000 <domain_curr_field+0x1c27fbe>
   75370: 911ef509     	add	x9, x8, #0x7bd
   75374: f81983b3     	stur	x19, [x29, #-0x68]
   75378: b000de28     	adrp	x8, 0x1c3a000 <domain_curr_field+0x1c39fbe>
   7537c: 9129a108     	add	x8, x8, #0xa68
   75380: a906a7e8     	stp	x8, x9, [sp, #0x68]
   75384: f90033ff     	str	xzr, [sp, #0x60]
   75388: b4000517     	cbz	x23, 0x75428 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xf8>
   7538c: d2800008     	mov	x8, #0x0                ; =0
   75390: d101a3b5     	sub	x21, x29, #0x68
   75394: 9101c3f6     	add	x22, sp, #0x70
   75398: 910183f8     	add	x24, sp, #0x60
   7539c: 910163f9     	add	x25, sp, #0x58
   753a0: 9101a3fa     	add	x26, sp, #0x68
   753a4: d503201f     	nop
   753a8: d503201f     	nop
   753ac: d503201f     	nop
   753b0: d503201f     	nop
   753b4: d503201f     	nop
   753b8: d503201f     	nop
   753bc: d503201f     	nop
   753c0: fc687a80     	ldr	d0, [x20, x8, lsl #3]
   753c4: fd002fe0     	str	d0, [sp, #0x58]
   753c8: 1e602000     	fcmp	d0, d0
   753cc: 540000c6     	b.vs	0x753e4 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xb4>
   753d0: 91000508     	add	x8, x8, #0x1
   753d4: f90033e8     	str	x8, [sp, #0x60]
   753d8: eb17011f     	cmp	x8, x23
   753dc: 54ffff23     	b.lo	0x753c0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x90>
   753e0: 14000011     	b	0x75424 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xf4>
   753e4: a901dbf5     	stp	x21, x22, [sp, #0x18]
   753e8: a902e7f8     	stp	x24, x25, [sp, #0x28]
   753ec: f9001ffa     	str	x26, [sp, #0x38]
   753f0: f90003e0     	str	x0, [sp]
   753f4: 910063e0     	add	x0, sp, #0x18
   753f8: aa0203fc     	mov	x28, x2
   753fc: aa0103fb     	mov	x27, x1
   75400: 9466aee0     	bl	0x1a20f80 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIN5Eigen6MatrixIdLin1ELi1ELi0ELin1ELi1EEEEEvPKcS7_RKT_EUldE_S5_JELPv0ELSC_0EEEvSA_S7_S7_RKT0_S7_DpRKT1_ENKUlvE_clEv>
   75404: f94003e0     	ldr	x0, [sp]
   75408: aa1b03e1     	mov	x1, x27
   7540c: aa1c03e2     	mov	x2, x28
   75410: f94033e8     	ldr	x8, [sp, #0x60]
   75414: 91000508     	add	x8, x8, #0x1
   75418: f90033e8     	str	x8, [sp, #0x60]
   7541c: eb17011f     	cmp	x8, x23
   75420: 54fffd03     	b.lo	0x753c0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x90>
   75424: fd402be0     	ldr	d0, [sp, #0x50]
   75428: f81983b3     	stur	x19, [x29, #-0x68]
   7542c: b000de28     	adrp	x8, 0x1c3a000 <domain_curr_field+0x1c39fbe>
   75430: 91388908     	add	x8, x8, #0xe22
   75434: f000dd89     	adrp	x9, 0x1c28000 <domain_curr_field+0x1c27fbe>
   75438: 911fe929     	add	x9, x9, #0x7fa
   7543c: a906a3e9     	stp	x9, x8, [sp, #0x68]
   75440: 9e660008     	fmov	x8, d0
   75444: 9240f908     	and	x8, x8, #0x7fffffffffffffff
   75448: d2effe09     	mov	x9, #0x7ff0000000000000 ; =9218868437227405312
   7544c: eb09011f     	cmp	x8, x9
   75450: 5400282a     	b.ge	0x75954 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x624>
   75454: f81983b3     	stur	x19, [x29, #-0x68]
   75458: f000dd88     	adrp	x8, 0x1c28000 <domain_curr_field+0x1c27fbe>
   7545c: 911fa909     	add	x9, x8, #0x7ea
   75460: f000dd88     	adrp	x8, 0x1c28000 <domain_curr_field+0x1c27fbe>
   75464: 91061d08     	add	x8, x8, #0x187
   75468: a906a7e8     	stp	x8, x9, [sp, #0x68]
   7546c: fd4027e0     	ldr	d0, [sp, #0x48]
   75470: 1e602008     	fcmp	d0, #0.0
   75474: 540028ed     	b.le	0x75990 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x660>
   75478: f9400416     	ldr	x22, [x0, #0x8]
   7547c: b4002a96     	cbz	x22, 0x759cc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x69c>
   75480: f9400028     	ldr	x8, [x1]
   75484: f9400049     	ldr	x9, [x2]
   75488: 910063ea     	add	x10, sp, #0x18
   7548c: a901abff     	stp	xzr, x10, [sp, #0x18]
   75490: 9100614a     	add	x10, x10, #0x18
   75494: a902ffe8     	stp	x8, xzr, [sp, #0x28]
   75498: a903a7ea     	stp	x10, x9, [sp, #0x38]
   7549c: fd4027e8     	ldr	d8, [sp, #0x48]
   754a0: b4000a97     	cbz	x23, 0x755f0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x2c0>
   754a4: d37cfee8     	lsr	x8, x23, #60
   754a8: b5002a88     	cbnz	x8, 0x759f8 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6c8>
   754ac: fd402be0     	ldr	d0, [sp, #0x50]
   754b0: 3d8003e0     	str	q0, [sp]
   754b4: d37df2f5     	lsl	x21, x23, #3
   754b8: aa1503e0     	mov	x0, x21
   754bc: 97ff3bf4     	bl	0x4448c <_malloc>
   754c0: b40029e0     	cbz	x0, 0x759fc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6cc>
   754c4: aa0003f3     	mov	x19, x0
   754c8: 1e6e1000     	fmov	d0, #1.00000000
   754cc: 927feaf8     	and	x24, x23, #0xffffffffffffffe
   754d0: 1e681802     	fdiv	d2, d0, d8
   754d4: f10006ff     	cmp	x23, #0x1
   754d8: 3dc003e3     	ldr	q3, [sp]
   754dc: 54000200     	b.eq	0x7551c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x1ec>
   754e0: d2800008     	mov	x8, #0x0                ; =0
   754e4: 4e080460     	dup.2d	v0, v3[0]
   754e8: aa1303e9     	mov	x9, x19
   754ec: aa1403ea     	mov	x10, x20
   754f0: d503201f     	nop
   754f4: d503201f     	nop
   754f8: d503201f     	nop
   754fc: d503201f     	nop
   75500: 3cc10541     	ldr	q1, [x10], #0x10
   75504: 4ee0d421     	fsub.2d	v1, v1, v0
   75508: 4fc29021     	fmul.2d	v1, v1, v2[0]
   7550c: 3c810521     	str	q1, [x9], #0x10
   75510: 91000908     	add	x8, x8, #0x2
   75514: eb18011f     	cmp	x8, x24
   75518: 54ffff43     	b.lo	0x75500 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x1d0>
   7551c: eb1802e8     	subs	x8, x23, x24
   75520: 540001c0     	b.eq	0x75558 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x228>
   75524: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   75528: 8b0a0289     	add	x9, x20, x10
   7552c: 8b0a026a     	add	x10, x19, x10
   75530: d503201f     	nop
   75534: d503201f     	nop
   75538: d503201f     	nop
   7553c: d503201f     	nop
   75540: fc408520     	ldr	d0, [x9], #0x8
   75544: 1e633800     	fsub	d0, d0, d3
   75548: 1e600840     	fmul	d0, d2, d0
   7554c: fc008540     	str	d0, [x10], #0x8
   75550: f1000508     	subs	x8, x8, #0x1
   75554: 54ffff61     	b.ne	0x75540 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x210>
   75558: 3d8003e2     	str	q2, [sp]
   7555c: aa1503e0     	mov	x0, x21
   75560: 97ff3bcb     	bl	0x4448c <_malloc>
   75564: b40024e0     	cbz	x0, 0x75a00 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6d0>
   75568: aa0003f4     	mov	x20, x0
   7556c: f10006ff     	cmp	x23, #0x1
   75570: 54000140     	b.eq	0x75598 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x268>
   75574: d2800008     	mov	x8, #0x0                ; =0
   75578: aa1403e9     	mov	x9, x20
   7557c: aa1303ea     	mov	x10, x19
   75580: 3cc10540     	ldr	q0, [x10], #0x10
   75584: 6e60dc00     	fmul.2d	v0, v0, v0
   75588: 3c810520     	str	q0, [x9], #0x10
   7558c: 91000908     	add	x8, x8, #0x2
   75590: eb18011f     	cmp	x8, x24
   75594: 54ffff63     	b.lo	0x75580 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x250>
   75598: eb1802e8     	subs	x8, x23, x24
   7559c: 540001c0     	b.eq	0x755d4 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x2a4>
   755a0: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   755a4: 8b0a0269     	add	x9, x19, x10
   755a8: 8b0a028a     	add	x10, x20, x10
   755ac: d503201f     	nop
   755b0: d503201f     	nop
   755b4: d503201f     	nop
   755b8: d503201f     	nop
   755bc: d503201f     	nop
   755c0: fc408520     	ldr	d0, [x9], #0x8
   755c4: 1e600800     	fmul	d0, d0, d0
   755c8: fc008540     	str	d0, [x10], #0x8
   755cc: f1000508     	subs	x8, x8, #0x1
   755d0: 54ffff81     	b.ne	0x755c0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x290>
   755d4: f10006df     	cmp	x22, #0x1
   755d8: 9a9fc6da     	csinc	x26, x22, xzr, gt
   755dc: 927ee6f9     	and	x25, x23, #0xffffffffffffffc
   755e0: f10006ff     	cmp	x23, #0x1
   755e4: 540002a1     	b.ne	0x75638 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x308>
   755e8: fd400289     	ldr	d9, [x20]
   755ec: 14000039     	b	0x756d0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3a0>
   755f0: 4ea81d00     	mov.16b	v0, v8
   755f4: 9467cb3a     	bl	0x1a682dc <domain_curr_field+0x1a6829a>
   755f8: d2800013     	mov	x19, #0x0               ; =0
   755fc: d2800014     	mov	x20, #0x0               ; =0
   75600: f10006df     	cmp	x22, #0x1
   75604: 9a9fc6c8     	csinc	x8, x22, xzr, gt
   75608: 9e630101     	ucvtf	d1, x8
   7560c: 1e608828     	fnmul	d8, d1, d0
   75610: f9001bff     	str	xzr, [sp, #0x30]
   75614: f9000fff     	str	xzr, [sp, #0x18]
   75618: 6f00e409     	movi.2d	v9, #0000000000000000
   7561c: f000f6e0     	adrp	x0, 0x1f54000 <_pool_freelist+0x118>
   75620: 9133e000     	add	x0, x0, #0xcf8
   75624: f9400008     	ldr	x8, [x0]
   75628: d63f0100     	blr	x8
   7562c: f9400008     	ldr	x8, [x0]
   75630: b5001648     	cbnz	x8, 0x758f8 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5c8>
   75634: 140000c1     	b	0x75938 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75638: 3dc00280     	ldr	q0, [x20]
   7563c: f10012ff     	cmp	x23, #0x4
   75640: 540002a3     	b.lo	0x75694 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x364>
   75644: 3dc00681     	ldr	q1, [x20, #0x10]
   75648: f10022ff     	cmp	x23, #0x8
   7564c: 54000183     	b.lo	0x7567c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x34c>
   75650: 9100c288     	add	x8, x20, #0x30
   75654: 52800089     	mov	w9, #0x4                ; =4
   75658: d503201f     	nop
   7565c: d503201f     	nop
   75660: ad7f8d02     	ldp	q2, q3, [x8, #-0x10]
   75664: 4e62d400     	fadd.2d	v0, v0, v2
   75668: 4e63d421     	fadd.2d	v1, v1, v3
   7566c: 91001129     	add	x9, x9, #0x4
   75670: 91008108     	add	x8, x8, #0x20
   75674: eb19013f     	cmp	x9, x25
   75678: 54ffff43     	b.lo	0x75660 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x330>
   7567c: 4e60d420     	fadd.2d	v0, v1, v0
   75680: eb19031f     	cmp	x24, x25
   75684: 54000089     	b.ls	0x75694 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x364>
   75688: d37df328     	lsl	x8, x25, #3
   7568c: 3ce86a81     	ldr	q1, [x20, x8]
   75690: 4e61d400     	fadd.2d	v0, v0, v1
   75694: 7e70d809     	faddp.2d	d9, v0
   75698: eb1802e8     	subs	x8, x23, x24
   7569c: 540001a0     	b.eq	0x756d0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3a0>
   756a0: d341eee9     	ubfx	x9, x23, #1, #59
   756a4: 8b091289     	add	x9, x20, x9, lsl #4
   756a8: d503201f     	nop
   756ac: d503201f     	nop
   756b0: d503201f     	nop
   756b4: d503201f     	nop
   756b8: d503201f     	nop
   756bc: d503201f     	nop
   756c0: fc408520     	ldr	d0, [x9], #0x8
   756c4: 1e602929     	fadd	d9, d9, d0
   756c8: f1000508     	subs	x8, x8, #0x1
   756cc: 54ffffa1     	b.ne	0x756c0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x390>
   756d0: aa1503e0     	mov	x0, x21
   756d4: 97ff3b6e     	bl	0x4448c <_malloc>
   756d8: b4001a60     	cbz	x0, 0x75a24 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6f4>
   756dc: aa0003f6     	mov	x22, x0
   756e0: 4ea81d00     	mov.16b	v0, v8
   756e4: 9467cafe     	bl	0x1a682dc <domain_curr_field+0x1a6829a>
   756e8: f10006ff     	cmp	x23, #0x1
   756ec: 3dc003e7     	ldr	q7, [sp]
   756f0: 54000140     	b.eq	0x75718 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3e8>
   756f4: d2800008     	mov	x8, #0x0                ; =0
   756f8: aa1603e9     	mov	x9, x22
   756fc: aa1303ea     	mov	x10, x19
   75700: 3cc10541     	ldr	q1, [x10], #0x10
   75704: 4fc79021     	fmul.2d	v1, v1, v7[0]
   75708: 3c810521     	str	q1, [x9], #0x10
   7570c: 91000908     	add	x8, x8, #0x2
   75710: eb18011f     	cmp	x8, x24
   75714: 54ffff63     	b.lo	0x75700 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3d0>
   75718: 1e7c1001     	fmov	d1, #-0.50000000
   7571c: 9e630342     	ucvtf	d2, x26
   75720: eb1802e8     	subs	x8, x23, x24
   75724: 54000180     	b.eq	0x75754 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x424>
   75728: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   7572c: 8b0a0269     	add	x9, x19, x10
   75730: 8b0a02ca     	add	x10, x22, x10
   75734: d503201f     	nop
   75738: d503201f     	nop
   7573c: d503201f     	nop
   75740: fc408523     	ldr	d3, [x9], #0x8
   75744: 1e6308e3     	fmul	d3, d7, d3
   75748: fc008543     	str	d3, [x10], #0x8
   7574c: f1000508     	subs	x8, x8, #0x1
   75750: 54ffff81     	b.ne	0x75740 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x410>
   75754: 1e610921     	fmul	d1, d9, d1
   75758: 1e620800     	fmul	d0, d0, d2
   7575c: f10006ff     	cmp	x23, #0x1
   75760: 540000e1     	b.ne	0x7577c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x44c>
   75764: fd400282     	ldr	d2, [x20]
   75768: 1e6208e2     	fmul	d2, d7, d2
   7576c: 1e673842     	fsub	d2, d2, d7
   75770: fd001be2     	str	d2, [sp, #0x30]
   75774: fd4002c9     	ldr	d9, [x22]
   75778: 14000056     	b	0x758d0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5a0>
   7577c: 4e0804e3     	dup.2d	v3, v7[0]
   75780: 3dc00282     	ldr	q2, [x20]
   75784: 4fc79042     	fmul.2d	v2, v2, v7[0]
   75788: 4ee3d442     	fsub.2d	v2, v2, v3
   7578c: f10012ff     	cmp	x23, #0x4
   75790: 540003e3     	b.lo	0x7580c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4dc>
   75794: 3dc00684     	ldr	q4, [x20, #0x10]
   75798: 4fc79084     	fmul.2d	v4, v4, v7[0]
   7579c: 4ee3d484     	fsub.2d	v4, v4, v3
   757a0: f10022ff     	cmp	x23, #0x8
   757a4: 54000243     	b.lo	0x757ec <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4bc>
   757a8: 9100c288     	add	x8, x20, #0x30
   757ac: 52800089     	mov	w9, #0x4                ; =4
   757b0: d503201f     	nop
   757b4: d503201f     	nop
   757b8: d503201f     	nop
   757bc: d503201f     	nop
   757c0: ad7f9905     	ldp	q5, q6, [x8, #-0x10]
   757c4: 4fc790a5     	fmul.2d	v5, v5, v7[0]
   757c8: 4ee3d4a5     	fsub.2d	v5, v5, v3
   757cc: 4e65d442     	fadd.2d	v2, v2, v5
   757d0: 4fc790c5     	fmul.2d	v5, v6, v7[0]
   757d4: 4ee3d4a5     	fsub.2d	v5, v5, v3
   757d8: 4e65d484     	fadd.2d	v4, v4, v5
   757dc: 91001129     	add	x9, x9, #0x4
   757e0: 91008108     	add	x8, x8, #0x20
   757e4: eb19013f     	cmp	x9, x25
   757e8: 54fffec3     	b.lo	0x757c0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x490>
   757ec: 4e62d482     	fadd.2d	v2, v4, v2
   757f0: eb19031f     	cmp	x24, x25
   757f4: 540000c9     	b.ls	0x7580c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4dc>
   757f8: d37df328     	lsl	x8, x25, #3
   757fc: 3ce86a84     	ldr	q4, [x20, x8]
   75800: 4fc79084     	fmul.2d	v4, v4, v7[0]
   75804: 4ee3d483     	fsub.2d	v3, v4, v3
   75808: 4e63d442     	fadd.2d	v2, v2, v3
   7580c: 7e70d842     	faddp.2d	d2, v2
   75810: eb1802e8     	subs	x8, x23, x24
   75814: 54000120     	b.eq	0x75838 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x508>
   75818: d341eee9     	ubfx	x9, x23, #1, #59
   7581c: 8b091289     	add	x9, x20, x9, lsl #4
   75820: fc408523     	ldr	d3, [x9], #0x8
   75824: 1e6308e3     	fmul	d3, d7, d3
   75828: 1e673863     	fsub	d3, d3, d7
   7582c: 1e632842     	fadd	d2, d2, d3
   75830: f1000508     	subs	x8, x8, #0x1
   75834: 54ffff61     	b.ne	0x75820 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4f0>
   75838: fd001be2     	str	d2, [sp, #0x30]
   7583c: 3dc002c2     	ldr	q2, [x22]
   75840: f10012ff     	cmp	x23, #0x4
   75844: 54000283     	b.lo	0x75894 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x564>
   75848: 3dc006c3     	ldr	q3, [x22, #0x10]
   7584c: f10022ff     	cmp	x23, #0x8
   75850: 54000163     	b.lo	0x7587c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x54c>
   75854: 9100c2c8     	add	x8, x22, #0x30
   75858: 52800089     	mov	w9, #0x4                ; =4
   7585c: d503201f     	nop
   75860: ad7f9504     	ldp	q4, q5, [x8, #-0x10]
   75864: 4e64d442     	fadd.2d	v2, v2, v4
   75868: 4e65d463     	fadd.2d	v3, v3, v5
   7586c: 91001129     	add	x9, x9, #0x4
   75870: 91008108     	add	x8, x8, #0x20
   75874: eb19013f     	cmp	x9, x25
   75878: 54ffff43     	b.lo	0x75860 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x530>
   7587c: 4e62d462     	fadd.2d	v2, v3, v2
   75880: eb19031f     	cmp	x24, x25
   75884: 54000089     	b.ls	0x75894 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x564>
   75888: d37df328     	lsl	x8, x25, #3
   7588c: 3ce86ac3     	ldr	q3, [x22, x8]
   75890: 4e63d442     	fadd.2d	v2, v2, v3
   75894: 7e70d849     	faddp.2d	d9, v2
   75898: eb1802e8     	subs	x8, x23, x24
   7589c: 540001a0     	b.eq	0x758d0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5a0>
   758a0: d341eee9     	ubfx	x9, x23, #1, #59
   758a4: 8b0912c9     	add	x9, x22, x9, lsl #4
   758a8: d503201f     	nop
   758ac: d503201f     	nop
   758b0: d503201f     	nop
   758b4: d503201f     	nop
   758b8: d503201f     	nop
   758bc: d503201f     	nop
   758c0: fc408522     	ldr	d2, [x9], #0x8
   758c4: 1e622929     	fadd	d9, d9, d2
   758c8: f1000508     	subs	x8, x8, #0x1
   758cc: 54ffffa1     	b.ne	0x758c0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x590>
   758d0: 1e603828     	fsub	d8, d1, d0
   758d4: fd000fe9     	str	d9, [sp, #0x18]
   758d8: aa1603e0     	mov	x0, x22
   758dc: 97ff3af9     	bl	0x444c0 <_free>
   758e0: f000f6e0     	adrp	x0, 0x1f54000 <_pool_freelist+0x118>
   758e4: 9133e000     	add	x0, x0, #0xcf8
   758e8: f9400008     	ldr	x8, [x0]
   758ec: d63f0100     	blr	x8
   758f0: f9400008     	ldr	x8, [x0]
   758f4: b4000228     	cbz	x8, 0x75938 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   758f8: 52800029     	mov	w9, #0x1                ; =1
   758fc: 39022109     	strb	w9, [x8, #0x88]
   75900: fd004108     	str	d8, [x8, #0x80]
   75904: f9400509     	ldr	x9, [x8, #0x8]
   75908: b40000a9     	cbz	x9, 0x7591c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5ec>
   7590c: f940250a     	ldr	x10, [x8, #0x48]
   75910: f100055f     	cmp	x10, #0x1
   75914: 54000041     	b.ne	0x7591c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5ec>
   75918: fd000129     	str	d9, [x9]
   7591c: f9400909     	ldr	x9, [x8, #0x10]
   75920: b40000c9     	cbz	x9, 0x75938 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75924: f9402908     	ldr	x8, [x8, #0x50]
   75928: f100051f     	cmp	x8, #0x1
   7592c: 54000061     	b.ne	0x75938 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75930: fd401be0     	ldr	d0, [sp, #0x30]
   75934: fd000120     	str	d0, [x9]
   75938: b4000074     	cbz	x20, 0x75944 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x614>
   7593c: aa1403e0     	mov	x0, x20
   75940: 97ff3ae0     	bl	0x444c0 <_free>
   75944: b4000473     	cbz	x19, 0x759d0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6a0>
   75948: aa1303e0     	mov	x0, x19
   7594c: 97ff3add     	bl	0x444c0 <_free>
   75950: 14000020     	b	0x759d0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6a0>
   75954: d101a3a8     	sub	x8, x29, #0x68
   75958: 9101c3e9     	add	x9, sp, #0x70
   7595c: a901a7e8     	stp	x8, x9, [sp, #0x18]
   75960: 910143e8     	add	x8, sp, #0x50
   75964: 9101a3e9     	add	x9, sp, #0x68
   75968: a902a7e8     	stp	x8, x9, [sp, #0x28]
   7596c: aa0003f5     	mov	x21, x0
   75970: 910063e0     	add	x0, sp, #0x18
   75974: aa0203f6     	mov	x22, x2
   75978: aa0103f8     	mov	x24, x1
   7597c: 94673ccd     	bl	0x1a44cb0 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIdEEvPKcS4_RKT_EUldE_dJELPv0EEEvS7_S4_S4_RKT0_S4_DpRKT1_ENKUlvE_clEv>
   75980: aa1503e0     	mov	x0, x21
   75984: aa1803e1     	mov	x1, x24
   75988: aa1603e2     	mov	x2, x22
   7598c: 17fffeb2     	b	0x75454 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x124>
   75990: d101a3a8     	sub	x8, x29, #0x68
   75994: 9101c3e9     	add	x9, sp, #0x70
   75998: a901a7e8     	stp	x8, x9, [sp, #0x18]
   7599c: 910123e8     	add	x8, sp, #0x48
   759a0: 9101a3e9     	add	x9, sp, #0x68
   759a4: a902a7e8     	stp	x8, x9, [sp, #0x28]
   759a8: aa0003f3     	mov	x19, x0
   759ac: 910063e0     	add	x0, sp, #0x18
   759b0: aa0203f5     	mov	x21, x2
   759b4: aa0103f6     	mov	x22, x1
   759b8: 94673cbe     	bl	0x1a44cb0 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIdEEvPKcS4_RKT_EUldE_dJELPv0EEEvS7_S4_S4_RKT0_S4_DpRKT1_ENKUlvE_clEv>
   759bc: aa1603e1     	mov	x1, x22
   759c0: aa1503e2     	mov	x2, x21
   759c4: f9400676     	ldr	x22, [x19, #0x8]
   759c8: b5ffd5d6     	cbnz	x22, 0x75480 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x150>
   759cc: 6f00e408     	movi.2d	v8, #0000000000000000
   759d0: 4ea81d00     	mov.16b	v0, v8
   759d4: a94e7bfd     	ldp	x29, x30, [sp, #0xe0]
   759d8: a94d4ff4     	ldp	x20, x19, [sp, #0xd0]
   759dc: a94c57f6     	ldp	x22, x21, [sp, #0xc0]
   759e0: a94b5ff8     	ldp	x24, x23, [sp, #0xb0]
   759e4: a94a67fa     	ldp	x26, x25, [sp, #0xa0]
   759e8: a9496ffc     	ldp	x28, x27, [sp, #0x90]
   759ec: 6d4823e9     	ldp	d9, d8, [sp, #0x80]
   759f0: 9103c3ff     	add	sp, sp, #0xf0
   759f4: d65f03c0     	ret
   759f8: 94669c3e     	bl	0x1a1caf0 <__ZZN6stanli9MirInterpIdE8eval_funERKNS_3mir4ExprEENKUlRKNS_7DataMap5EntryEbE_clES9_b.cold.2>
   759fc: 94669c3d     	bl	0x1a1caf0 <__ZZN6stanli9MirInterpIdE8eval_funERKNS_3mir4ExprEENKUlRKNS_7DataMap5EntryEbE_clES9_b.cold.2>
   75a00: 52800100     	mov	w0, #0x8                ; =8
   75a04: 9467c89b     	bl	0x1a67c70 <domain_curr_field+0x1a67c2e>
   75a08: 9467c87c     	bl	0x1a67bf8 <domain_curr_field+0x1a67bb6>
   75a0c: f000e2e1     	adrp	x1, 0x1cd4000 <domain_curr_field+0x1cd3fbe>
   75a10: f9407421     	ldr	x1, [x1, #0xe8]
   75a14: f000e2e2     	adrp	x2, 0x1cd4000 <domain_curr_field+0x1cd3fbe>
   75a18: f9405842     	ldr	x2, [x2, #0xb0]
   75a1c: 9467c8b0     	bl	0x1a67cdc <domain_curr_field+0x1a67c9a>
   75a20: 14000009     	b	0x75a44 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x714>
   75a24: 52800100     	mov	w0, #0x8                ; =8
   75a28: 9467c892     	bl	0x1a67c70 <domain_curr_field+0x1a67c2e>
   75a2c: 9467c873     	bl	0x1a67bf8 <domain_curr_field+0x1a67bb6>
   75a30: f000e2e1     	adrp	x1, 0x1cd4000 <domain_curr_field+0x1cd3fbe>
   75a34: f9407421     	ldr	x1, [x1, #0xe8]
   75a38: f000e2e2     	adrp	x2, 0x1cd4000 <domain_curr_field+0x1cd3fbe>
   75a3c: f9405842     	ldr	x2, [x2, #0xb0]
   75a40: 9467c8a7     	bl	0x1a67cdc <domain_curr_field+0x1a67c9a>
   75a44: d4200020     	brk	#0x1
   75a48: aa0003f5     	mov	x21, x0
   75a4c: aa1403e0     	mov	x0, x20
   75a50: 97ff3a9c     	bl	0x444c0 <_free>
   75a54: aa1303e0     	mov	x0, x19
   75a58: 97ff3a9a     	bl	0x444c0 <_free>
   75a5c: aa1503e0     	mov	x0, x21
   75a60: 9467c722     	bl	0x1a676e8 <domain_curr_field+0x1a676a6>
   75a64: aa0003f5     	mov	x21, x0
   75a68: aa1303e0     	mov	x0, x19
   75a6c: 97ff3a95     	bl	0x444c0 <_free>
   75a70: aa1503e0     	mov	x0, x21
   75a74: 9467c71d     	bl	0x1a676e8 <domain_curr_field+0x1a676a6>


/tmp/stanli-allocator-rollout.l6S5ZY/apple-ablation-libs/private-on/libstanli_allocator_benchmark.dylib:	file format mach-o arm64

Disassembly of section __TEXT,__text:

0000000000075310 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_>:
   75310: d103c3ff     	sub	sp, sp, #0xf0
   75314: 6d0823e9     	stp	d9, d8, [sp, #0x80]
   75318: a9096ffc     	stp	x28, x27, [sp, #0x90]
   7531c: a90a67fa     	stp	x26, x25, [sp, #0xa0]
   75320: a90b5ff8     	stp	x24, x23, [sp, #0xb0]
   75324: a90c57f6     	stp	x22, x21, [sp, #0xc0]
   75328: a90d4ff4     	stp	x20, x19, [sp, #0xd0]
   7532c: a90e7bfd     	stp	x29, x30, [sp, #0xe0]
   75330: 910383fd     	add	x29, sp, #0xe0
   75334: a9405c14     	ldp	x20, x23, [x0]
   75338: fd400020     	ldr	d0, [x1]
   7533c: fd400041     	ldr	d1, [x2]
   75340: 6d0483e1     	stp	d1, d0, [sp, #0x48]
   75344: b000ddb3     	adrp	x19, 0x1c2a000 <domain_curr_field+0x1c29fbe>
   75348: 91307673     	add	x19, x19, #0xc1d
   7534c: f000dd88     	adrp	x8, 0x1c28000 <domain_curr_field+0x1c27fbe>
   75350: 911e3509     	add	x9, x8, #0x78d
   75354: f81983b3     	stur	x19, [x29, #-0x68]
   75358: b000de28     	adrp	x8, 0x1c3a000 <domain_curr_field+0x1c39fbe>
   7535c: 9128e108     	add	x8, x8, #0xa38
   75360: a906a7e8     	stp	x8, x9, [sp, #0x68]
   75364: f90033ff     	str	xzr, [sp, #0x60]
   75368: b4000517     	cbz	x23, 0x75408 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xf8>
   7536c: d2800008     	mov	x8, #0x0                ; =0
   75370: d101a3b5     	sub	x21, x29, #0x68
   75374: 9101c3f6     	add	x22, sp, #0x70
   75378: 910183f8     	add	x24, sp, #0x60
   7537c: 910163f9     	add	x25, sp, #0x58
   75380: 9101a3fa     	add	x26, sp, #0x68
   75384: d503201f     	nop
   75388: d503201f     	nop
   7538c: d503201f     	nop
   75390: d503201f     	nop
   75394: d503201f     	nop
   75398: d503201f     	nop
   7539c: d503201f     	nop
   753a0: fc687a80     	ldr	d0, [x20, x8, lsl #3]
   753a4: fd002fe0     	str	d0, [sp, #0x58]
   753a8: 1e602000     	fcmp	d0, d0
   753ac: 540000c6     	b.vs	0x753c4 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xb4>
   753b0: 91000508     	add	x8, x8, #0x1
   753b4: f90033e8     	str	x8, [sp, #0x60]
   753b8: eb17011f     	cmp	x8, x23
   753bc: 54ffff23     	b.lo	0x753a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x90>
   753c0: 14000011     	b	0x75404 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0xf4>
   753c4: a901dbf5     	stp	x21, x22, [sp, #0x18]
   753c8: a902e7f8     	stp	x24, x25, [sp, #0x28]
   753cc: f9001ffa     	str	x26, [sp, #0x38]
   753d0: f90003e0     	str	x0, [sp]
   753d4: 910063e0     	add	x0, sp, #0x18
   753d8: aa0203fc     	mov	x28, x2
   753dc: aa0103fb     	mov	x27, x1
   753e0: 9466aee8     	bl	0x1a20f80 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIN5Eigen6MatrixIdLin1ELi1ELi0ELin1ELi1EEEEEvPKcS7_RKT_EUldE_S5_JELPv0ELSC_0EEEvSA_S7_S7_RKT0_S7_DpRKT1_ENKUlvE_clEv>
   753e4: f94003e0     	ldr	x0, [sp]
   753e8: aa1b03e1     	mov	x1, x27
   753ec: aa1c03e2     	mov	x2, x28
   753f0: f94033e8     	ldr	x8, [sp, #0x60]
   753f4: 91000508     	add	x8, x8, #0x1
   753f8: f90033e8     	str	x8, [sp, #0x60]
   753fc: eb17011f     	cmp	x8, x23
   75400: 54fffd03     	b.lo	0x753a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x90>
   75404: fd402be0     	ldr	d0, [sp, #0x50]
   75408: f81983b3     	stur	x19, [x29, #-0x68]
   7540c: b000de28     	adrp	x8, 0x1c3a000 <domain_curr_field+0x1c39fbe>
   75410: 9137c908     	add	x8, x8, #0xdf2
   75414: f000dd89     	adrp	x9, 0x1c28000 <domain_curr_field+0x1c27fbe>
   75418: 911f2929     	add	x9, x9, #0x7ca
   7541c: a906a3e9     	stp	x9, x8, [sp, #0x68]
   75420: 9e660008     	fmov	x8, d0
   75424: 9240f908     	and	x8, x8, #0x7fffffffffffffff
   75428: d2effe09     	mov	x9, #0x7ff0000000000000 ; =9218868437227405312
   7542c: eb09011f     	cmp	x8, x9
   75430: 5400282a     	b.ge	0x75934 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x624>
   75434: f81983b3     	stur	x19, [x29, #-0x68]
   75438: f000dd88     	adrp	x8, 0x1c28000 <domain_curr_field+0x1c27fbe>
   7543c: 911ee909     	add	x9, x8, #0x7ba
   75440: f000dd88     	adrp	x8, 0x1c28000 <domain_curr_field+0x1c27fbe>
   75444: 91055d08     	add	x8, x8, #0x157
   75448: a906a7e8     	stp	x8, x9, [sp, #0x68]
   7544c: fd4027e0     	ldr	d0, [sp, #0x48]
   75450: 1e602008     	fcmp	d0, #0.0
   75454: 540028ed     	b.le	0x75970 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x660>
   75458: f9400416     	ldr	x22, [x0, #0x8]
   7545c: b4002a96     	cbz	x22, 0x759ac <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x69c>
   75460: f9400028     	ldr	x8, [x1]
   75464: f9400049     	ldr	x9, [x2]
   75468: 910063ea     	add	x10, sp, #0x18
   7546c: a901abff     	stp	xzr, x10, [sp, #0x18]
   75470: 9100614a     	add	x10, x10, #0x18
   75474: a902ffe8     	stp	x8, xzr, [sp, #0x28]
   75478: a903a7ea     	stp	x10, x9, [sp, #0x38]
   7547c: fd4027e8     	ldr	d8, [sp, #0x48]
   75480: b4000a97     	cbz	x23, 0x755d0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x2c0>
   75484: d37cfee8     	lsr	x8, x23, #60
   75488: b5002a88     	cbnz	x8, 0x759d8 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6c8>
   7548c: fd402be0     	ldr	d0, [sp, #0x50]
   75490: 3d8003e0     	str	q0, [sp]
   75494: d37df2f5     	lsl	x21, x23, #3
   75498: aa1503e0     	mov	x0, x21
   7549c: 97ff3bfc     	bl	0x4448c <_malloc>
   754a0: b40029e0     	cbz	x0, 0x759dc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6cc>
   754a4: aa0003f3     	mov	x19, x0
   754a8: 1e6e1000     	fmov	d0, #1.00000000
   754ac: 927feaf8     	and	x24, x23, #0xffffffffffffffe
   754b0: 1e681802     	fdiv	d2, d0, d8
   754b4: f10006ff     	cmp	x23, #0x1
   754b8: 3dc003e3     	ldr	q3, [sp]
   754bc: 54000200     	b.eq	0x754fc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x1ec>
   754c0: d2800008     	mov	x8, #0x0                ; =0
   754c4: 4e080460     	dup.2d	v0, v3[0]
   754c8: aa1303e9     	mov	x9, x19
   754cc: aa1403ea     	mov	x10, x20
   754d0: d503201f     	nop
   754d4: d503201f     	nop
   754d8: d503201f     	nop
   754dc: d503201f     	nop
   754e0: 3cc10541     	ldr	q1, [x10], #0x10
   754e4: 4ee0d421     	fsub.2d	v1, v1, v0
   754e8: 4fc29021     	fmul.2d	v1, v1, v2[0]
   754ec: 3c810521     	str	q1, [x9], #0x10
   754f0: 91000908     	add	x8, x8, #0x2
   754f4: eb18011f     	cmp	x8, x24
   754f8: 54ffff43     	b.lo	0x754e0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x1d0>
   754fc: eb1802e8     	subs	x8, x23, x24
   75500: 540001c0     	b.eq	0x75538 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x228>
   75504: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   75508: 8b0a0289     	add	x9, x20, x10
   7550c: 8b0a026a     	add	x10, x19, x10
   75510: d503201f     	nop
   75514: d503201f     	nop
   75518: d503201f     	nop
   7551c: d503201f     	nop
   75520: fc408520     	ldr	d0, [x9], #0x8
   75524: 1e633800     	fsub	d0, d0, d3
   75528: 1e600840     	fmul	d0, d2, d0
   7552c: fc008540     	str	d0, [x10], #0x8
   75530: f1000508     	subs	x8, x8, #0x1
   75534: 54ffff61     	b.ne	0x75520 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x210>
   75538: 3d8003e2     	str	q2, [sp]
   7553c: aa1503e0     	mov	x0, x21
   75540: 97ff3bd3     	bl	0x4448c <_malloc>
   75544: b40024e0     	cbz	x0, 0x759e0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6d0>
   75548: aa0003f4     	mov	x20, x0
   7554c: f10006ff     	cmp	x23, #0x1
   75550: 54000140     	b.eq	0x75578 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x268>
   75554: d2800008     	mov	x8, #0x0                ; =0
   75558: aa1403e9     	mov	x9, x20
   7555c: aa1303ea     	mov	x10, x19
   75560: 3cc10540     	ldr	q0, [x10], #0x10
   75564: 6e60dc00     	fmul.2d	v0, v0, v0
   75568: 3c810520     	str	q0, [x9], #0x10
   7556c: 91000908     	add	x8, x8, #0x2
   75570: eb18011f     	cmp	x8, x24
   75574: 54ffff63     	b.lo	0x75560 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x250>
   75578: eb1802e8     	subs	x8, x23, x24
   7557c: 540001c0     	b.eq	0x755b4 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x2a4>
   75580: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   75584: 8b0a0269     	add	x9, x19, x10
   75588: 8b0a028a     	add	x10, x20, x10
   7558c: d503201f     	nop
   75590: d503201f     	nop
   75594: d503201f     	nop
   75598: d503201f     	nop
   7559c: d503201f     	nop
   755a0: fc408520     	ldr	d0, [x9], #0x8
   755a4: 1e600800     	fmul	d0, d0, d0
   755a8: fc008540     	str	d0, [x10], #0x8
   755ac: f1000508     	subs	x8, x8, #0x1
   755b0: 54ffff81     	b.ne	0x755a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x290>
   755b4: f10006df     	cmp	x22, #0x1
   755b8: 9a9fc6da     	csinc	x26, x22, xzr, gt
   755bc: 927ee6f9     	and	x25, x23, #0xffffffffffffffc
   755c0: f10006ff     	cmp	x23, #0x1
   755c4: 540002a1     	b.ne	0x75618 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x308>
   755c8: fd400289     	ldr	d9, [x20]
   755cc: 14000039     	b	0x756b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3a0>
   755d0: 4ea81d00     	mov.16b	v0, v8
   755d4: 9467cb42     	bl	0x1a682dc <domain_curr_field+0x1a6829a>
   755d8: d2800013     	mov	x19, #0x0               ; =0
   755dc: d2800014     	mov	x20, #0x0               ; =0
   755e0: f10006df     	cmp	x22, #0x1
   755e4: 9a9fc6c8     	csinc	x8, x22, xzr, gt
   755e8: 9e630101     	ucvtf	d1, x8
   755ec: 1e608828     	fnmul	d8, d1, d0
   755f0: f9001bff     	str	xzr, [sp, #0x30]
   755f4: f9000fff     	str	xzr, [sp, #0x18]
   755f8: 6f00e409     	movi.2d	v9, #0000000000000000
   755fc: f000f6e0     	adrp	x0, 0x1f54000 <_pool_freelist+0x158>
   75600: 9132e000     	add	x0, x0, #0xcb8
   75604: f9400008     	ldr	x8, [x0]
   75608: d63f0100     	blr	x8
   7560c: f9400008     	ldr	x8, [x0]
   75610: b5001648     	cbnz	x8, 0x758d8 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5c8>
   75614: 140000c1     	b	0x75918 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75618: 3dc00280     	ldr	q0, [x20]
   7561c: f10012ff     	cmp	x23, #0x4
   75620: 540002a3     	b.lo	0x75674 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x364>
   75624: 3dc00681     	ldr	q1, [x20, #0x10]
   75628: f10022ff     	cmp	x23, #0x8
   7562c: 54000183     	b.lo	0x7565c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x34c>
   75630: 9100c288     	add	x8, x20, #0x30
   75634: 52800089     	mov	w9, #0x4                ; =4
   75638: d503201f     	nop
   7563c: d503201f     	nop
   75640: ad7f8d02     	ldp	q2, q3, [x8, #-0x10]
   75644: 4e62d400     	fadd.2d	v0, v0, v2
   75648: 4e63d421     	fadd.2d	v1, v1, v3
   7564c: 91001129     	add	x9, x9, #0x4
   75650: 91008108     	add	x8, x8, #0x20
   75654: eb19013f     	cmp	x9, x25
   75658: 54ffff43     	b.lo	0x75640 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x330>
   7565c: 4e60d420     	fadd.2d	v0, v1, v0
   75660: eb19031f     	cmp	x24, x25
   75664: 54000089     	b.ls	0x75674 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x364>
   75668: d37df328     	lsl	x8, x25, #3
   7566c: 3ce86a81     	ldr	q1, [x20, x8]
   75670: 4e61d400     	fadd.2d	v0, v0, v1
   75674: 7e70d809     	faddp.2d	d9, v0
   75678: eb1802e8     	subs	x8, x23, x24
   7567c: 540001a0     	b.eq	0x756b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3a0>
   75680: d341eee9     	ubfx	x9, x23, #1, #59
   75684: 8b091289     	add	x9, x20, x9, lsl #4
   75688: d503201f     	nop
   7568c: d503201f     	nop
   75690: d503201f     	nop
   75694: d503201f     	nop
   75698: d503201f     	nop
   7569c: d503201f     	nop
   756a0: fc408520     	ldr	d0, [x9], #0x8
   756a4: 1e602929     	fadd	d9, d9, d0
   756a8: f1000508     	subs	x8, x8, #0x1
   756ac: 54ffffa1     	b.ne	0x756a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x390>
   756b0: aa1503e0     	mov	x0, x21
   756b4: 97ff3b76     	bl	0x4448c <_malloc>
   756b8: b4001a60     	cbz	x0, 0x75a04 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6f4>
   756bc: aa0003f6     	mov	x22, x0
   756c0: 4ea81d00     	mov.16b	v0, v8
   756c4: 9467cb06     	bl	0x1a682dc <domain_curr_field+0x1a6829a>
   756c8: f10006ff     	cmp	x23, #0x1
   756cc: 3dc003e7     	ldr	q7, [sp]
   756d0: 54000140     	b.eq	0x756f8 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3e8>
   756d4: d2800008     	mov	x8, #0x0                ; =0
   756d8: aa1603e9     	mov	x9, x22
   756dc: aa1303ea     	mov	x10, x19
   756e0: 3cc10541     	ldr	q1, [x10], #0x10
   756e4: 4fc79021     	fmul.2d	v1, v1, v7[0]
   756e8: 3c810521     	str	q1, [x9], #0x10
   756ec: 91000908     	add	x8, x8, #0x2
   756f0: eb18011f     	cmp	x8, x24
   756f4: 54ffff63     	b.lo	0x756e0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x3d0>
   756f8: 1e7c1001     	fmov	d1, #-0.50000000
   756fc: 9e630342     	ucvtf	d2, x26
   75700: eb1802e8     	subs	x8, x23, x24
   75704: 54000180     	b.eq	0x75734 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x424>
   75708: 927ceaaa     	and	x10, x21, #0x7ffffffffffffff0
   7570c: 8b0a0269     	add	x9, x19, x10
   75710: 8b0a02ca     	add	x10, x22, x10
   75714: d503201f     	nop
   75718: d503201f     	nop
   7571c: d503201f     	nop
   75720: fc408523     	ldr	d3, [x9], #0x8
   75724: 1e6308e3     	fmul	d3, d7, d3
   75728: fc008543     	str	d3, [x10], #0x8
   7572c: f1000508     	subs	x8, x8, #0x1
   75730: 54ffff81     	b.ne	0x75720 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x410>
   75734: 1e610921     	fmul	d1, d9, d1
   75738: 1e620800     	fmul	d0, d0, d2
   7573c: f10006ff     	cmp	x23, #0x1
   75740: 540000e1     	b.ne	0x7575c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x44c>
   75744: fd400282     	ldr	d2, [x20]
   75748: 1e6208e2     	fmul	d2, d7, d2
   7574c: 1e673842     	fsub	d2, d2, d7
   75750: fd001be2     	str	d2, [sp, #0x30]
   75754: fd4002c9     	ldr	d9, [x22]
   75758: 14000056     	b	0x758b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5a0>
   7575c: 4e0804e3     	dup.2d	v3, v7[0]
   75760: 3dc00282     	ldr	q2, [x20]
   75764: 4fc79042     	fmul.2d	v2, v2, v7[0]
   75768: 4ee3d442     	fsub.2d	v2, v2, v3
   7576c: f10012ff     	cmp	x23, #0x4
   75770: 540003e3     	b.lo	0x757ec <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4dc>
   75774: 3dc00684     	ldr	q4, [x20, #0x10]
   75778: 4fc79084     	fmul.2d	v4, v4, v7[0]
   7577c: 4ee3d484     	fsub.2d	v4, v4, v3
   75780: f10022ff     	cmp	x23, #0x8
   75784: 54000243     	b.lo	0x757cc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4bc>
   75788: 9100c288     	add	x8, x20, #0x30
   7578c: 52800089     	mov	w9, #0x4                ; =4
   75790: d503201f     	nop
   75794: d503201f     	nop
   75798: d503201f     	nop
   7579c: d503201f     	nop
   757a0: ad7f9905     	ldp	q5, q6, [x8, #-0x10]
   757a4: 4fc790a5     	fmul.2d	v5, v5, v7[0]
   757a8: 4ee3d4a5     	fsub.2d	v5, v5, v3
   757ac: 4e65d442     	fadd.2d	v2, v2, v5
   757b0: 4fc790c5     	fmul.2d	v5, v6, v7[0]
   757b4: 4ee3d4a5     	fsub.2d	v5, v5, v3
   757b8: 4e65d484     	fadd.2d	v4, v4, v5
   757bc: 91001129     	add	x9, x9, #0x4
   757c0: 91008108     	add	x8, x8, #0x20
   757c4: eb19013f     	cmp	x9, x25
   757c8: 54fffec3     	b.lo	0x757a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x490>
   757cc: 4e62d482     	fadd.2d	v2, v4, v2
   757d0: eb19031f     	cmp	x24, x25
   757d4: 540000c9     	b.ls	0x757ec <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4dc>
   757d8: d37df328     	lsl	x8, x25, #3
   757dc: 3ce86a84     	ldr	q4, [x20, x8]
   757e0: 4fc79084     	fmul.2d	v4, v4, v7[0]
   757e4: 4ee3d483     	fsub.2d	v3, v4, v3
   757e8: 4e63d442     	fadd.2d	v2, v2, v3
   757ec: 7e70d842     	faddp.2d	d2, v2
   757f0: eb1802e8     	subs	x8, x23, x24
   757f4: 54000120     	b.eq	0x75818 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x508>
   757f8: d341eee9     	ubfx	x9, x23, #1, #59
   757fc: 8b091289     	add	x9, x20, x9, lsl #4
   75800: fc408523     	ldr	d3, [x9], #0x8
   75804: 1e6308e3     	fmul	d3, d7, d3
   75808: 1e673863     	fsub	d3, d3, d7
   7580c: 1e632842     	fadd	d2, d2, d3
   75810: f1000508     	subs	x8, x8, #0x1
   75814: 54ffff61     	b.ne	0x75800 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x4f0>
   75818: fd001be2     	str	d2, [sp, #0x30]
   7581c: 3dc002c2     	ldr	q2, [x22]
   75820: f10012ff     	cmp	x23, #0x4
   75824: 54000283     	b.lo	0x75874 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x564>
   75828: 3dc006c3     	ldr	q3, [x22, #0x10]
   7582c: f10022ff     	cmp	x23, #0x8
   75830: 54000163     	b.lo	0x7585c <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x54c>
   75834: 9100c2c8     	add	x8, x22, #0x30
   75838: 52800089     	mov	w9, #0x4                ; =4
   7583c: d503201f     	nop
   75840: ad7f9504     	ldp	q4, q5, [x8, #-0x10]
   75844: 4e64d442     	fadd.2d	v2, v2, v4
   75848: 4e65d463     	fadd.2d	v3, v3, v5
   7584c: 91001129     	add	x9, x9, #0x4
   75850: 91008108     	add	x8, x8, #0x20
   75854: eb19013f     	cmp	x9, x25
   75858: 54ffff43     	b.lo	0x75840 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x530>
   7585c: 4e62d462     	fadd.2d	v2, v3, v2
   75860: eb19031f     	cmp	x24, x25
   75864: 54000089     	b.ls	0x75874 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x564>
   75868: d37df328     	lsl	x8, x25, #3
   7586c: 3ce86ac3     	ldr	q3, [x22, x8]
   75870: 4e63d442     	fadd.2d	v2, v2, v3
   75874: 7e70d849     	faddp.2d	d9, v2
   75878: eb1802e8     	subs	x8, x23, x24
   7587c: 540001a0     	b.eq	0x758b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5a0>
   75880: d341eee9     	ubfx	x9, x23, #1, #59
   75884: 8b0912c9     	add	x9, x22, x9, lsl #4
   75888: d503201f     	nop
   7588c: d503201f     	nop
   75890: d503201f     	nop
   75894: d503201f     	nop
   75898: d503201f     	nop
   7589c: d503201f     	nop
   758a0: fc408522     	ldr	d2, [x9], #0x8
   758a4: 1e622929     	fadd	d9, d9, d2
   758a8: f1000508     	subs	x8, x8, #0x1
   758ac: 54ffffa1     	b.ne	0x758a0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x590>
   758b0: 1e603828     	fsub	d8, d1, d0
   758b4: fd000fe9     	str	d9, [sp, #0x18]
   758b8: aa1603e0     	mov	x0, x22
   758bc: 97ff3af6     	bl	0x44494 <_free>
   758c0: f000f6e0     	adrp	x0, 0x1f54000 <_pool_freelist+0x158>
   758c4: 9132e000     	add	x0, x0, #0xcb8
   758c8: f9400008     	ldr	x8, [x0]
   758cc: d63f0100     	blr	x8
   758d0: f9400008     	ldr	x8, [x0]
   758d4: b4000228     	cbz	x8, 0x75918 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   758d8: 52800029     	mov	w9, #0x1                ; =1
   758dc: 39022109     	strb	w9, [x8, #0x88]
   758e0: fd004108     	str	d8, [x8, #0x80]
   758e4: f9400509     	ldr	x9, [x8, #0x8]
   758e8: b40000a9     	cbz	x9, 0x758fc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5ec>
   758ec: f940250a     	ldr	x10, [x8, #0x48]
   758f0: f100055f     	cmp	x10, #0x1
   758f4: 54000041     	b.ne	0x758fc <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x5ec>
   758f8: fd000129     	str	d9, [x9]
   758fc: f9400909     	ldr	x9, [x8, #0x10]
   75900: b40000c9     	cbz	x9, 0x75918 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75904: f9402908     	ldr	x8, [x8, #0x50]
   75908: f100051f     	cmp	x8, #0x1
   7590c: 54000061     	b.ne	0x75918 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x608>
   75910: fd401be0     	ldr	d0, [sp, #0x30]
   75914: fd000120     	str	d0, [x9]
   75918: b4000074     	cbz	x20, 0x75924 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x614>
   7591c: aa1403e0     	mov	x0, x20
   75920: 97ff3add     	bl	0x44494 <_free>
   75924: b4000473     	cbz	x19, 0x759b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6a0>
   75928: aa1303e0     	mov	x0, x19
   7592c: 97ff3ada     	bl	0x44494 <_free>
   75930: 14000020     	b	0x759b0 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x6a0>
   75934: d101a3a8     	sub	x8, x29, #0x68
   75938: 9101c3e9     	add	x9, sp, #0x70
   7593c: a901a7e8     	stp	x8, x9, [sp, #0x18]
   75940: 910143e8     	add	x8, sp, #0x50
   75944: 9101a3e9     	add	x9, sp, #0x68
   75948: a902a7e8     	stp	x8, x9, [sp, #0x28]
   7594c: aa0003f5     	mov	x21, x0
   75950: 910063e0     	add	x0, sp, #0x18
   75954: aa0203f6     	mov	x22, x2
   75958: aa0103f8     	mov	x24, x1
   7595c: 94673cd5     	bl	0x1a44cb0 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIdEEvPKcS4_RKT_EUldE_dJELPv0EEEvS7_S4_S4_RKT0_S4_DpRKT1_ENKUlvE_clEv>
   75960: aa1503e0     	mov	x0, x21
   75964: aa1803e1     	mov	x1, x24
   75968: aa1603e2     	mov	x2, x22
   7596c: 17fffeb2     	b	0x75434 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x124>
   75970: d101a3a8     	sub	x8, x29, #0x68
   75974: 9101c3e9     	add	x9, sp, #0x70
   75978: a901a7e8     	stp	x8, x9, [sp, #0x18]
   7597c: 910123e8     	add	x8, sp, #0x48
   75980: 9101a3e9     	add	x9, sp, #0x68
   75984: a902a7e8     	stp	x8, x9, [sp, #0x28]
   75988: aa0003f3     	mov	x19, x0
   7598c: 910063e0     	add	x0, sp, #0x18
   75990: aa0203f5     	mov	x21, x2
   75994: aa0103f6     	mov	x22, x1
   75998: 94673cc6     	bl	0x1a44cb0 <__ZZN4stan4math17elementwise_checkIZNS0_21check_positive_finiteIdEEvPKcS4_RKT_EUldE_dJELPv0EEEvS7_S4_S4_RKT0_S4_DpRKT1_ENKUlvE_clEv>
   7599c: aa1603e1     	mov	x1, x22
   759a0: aa1503e2     	mov	x2, x21
   759a4: f9400676     	ldr	x22, [x19, #0x8]
   759a8: b5ffd5d6     	cbnz	x22, 0x75460 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x150>
   759ac: 6f00e408     	movi.2d	v8, #0000000000000000
   759b0: 4ea81d00     	mov.16b	v0, v8
   759b4: a94e7bfd     	ldp	x29, x30, [sp, #0xe0]
   759b8: a94d4ff4     	ldp	x20, x19, [sp, #0xd0]
   759bc: a94c57f6     	ldp	x22, x21, [sp, #0xc0]
   759c0: a94b5ff8     	ldp	x24, x23, [sp, #0xb0]
   759c4: a94a67fa     	ldp	x26, x25, [sp, #0xa0]
   759c8: a9496ffc     	ldp	x28, x27, [sp, #0x90]
   759cc: 6d4823e9     	ldp	d9, d8, [sp, #0x80]
   759d0: 9103c3ff     	add	sp, sp, #0xf0
   759d4: d65f03c0     	ret
   759d8: 94669c46     	bl	0x1a1caf0 <__ZZN6stanli9MirInterpIdE8eval_funERKNS_3mir4ExprEENKUlRKNS_7DataMap5EntryEbE_clES9_b.cold.2>
   759dc: 94669c45     	bl	0x1a1caf0 <__ZZN6stanli9MirInterpIdE8eval_funERKNS_3mir4ExprEENKUlRKNS_7DataMap5EntryEbE_clES9_b.cold.2>
   759e0: 52800100     	mov	w0, #0x8                ; =8
   759e4: 9467c8a3     	bl	0x1a67c70 <domain_curr_field+0x1a67c2e>
   759e8: 9467c884     	bl	0x1a67bf8 <domain_curr_field+0x1a67bb6>
   759ec: f000e2e1     	adrp	x1, 0x1cd4000 <domain_curr_field+0x1cd3fbe>
   759f0: f9407421     	ldr	x1, [x1, #0xe8]
   759f4: f000e2e2     	adrp	x2, 0x1cd4000 <domain_curr_field+0x1cd3fbe>
   759f8: f9405842     	ldr	x2, [x2, #0xb0]
   759fc: 9467c8b8     	bl	0x1a67cdc <domain_curr_field+0x1a67c9a>
   75a00: 14000009     	b	0x75a24 <__ZN4stan4math11normal_lpdfILb1ERKN5Eigen3MapIKNS2_6MatrixIdLin1ELi1ELi0ELin1ELi1EEELi0ENS2_6StrideILi0ELi0EEEEERKN6stanli4rvarESF_LPv0EEENS_11return_typeIJT0_T1_T2_EE4typeEOSI_OSJ_OSK_+0x714>
   75a04: 52800100     	mov	w0, #0x8                ; =8
   75a08: 9467c89a     	bl	0x1a67c70 <domain_curr_field+0x1a67c2e>
   75a0c: 9467c87b     	bl	0x1a67bf8 <domain_curr_field+0x1a67bb6>
   75a10: f000e2e1     	adrp	x1, 0x1cd4000 <domain_curr_field+0x1cd3fbe>
   75a14: f9407421     	ldr	x1, [x1, #0xe8]
   75a18: f000e2e2     	adrp	x2, 0x1cd4000 <domain_curr_field+0x1cd3fbe>
   75a1c: f9405842     	ldr	x2, [x2, #0xb0]
   75a20: 9467c8af     	bl	0x1a67cdc <domain_curr_field+0x1a67c9a>
   75a24: d4200020     	brk	#0x1
   75a28: aa0003f5     	mov	x21, x0
   75a2c: aa1403e0     	mov	x0, x20
   75a30: 97ff3a99     	bl	0x44494 <_free>
   75a34: aa1303e0     	mov	x0, x19
   75a38: 97ff3a97     	bl	0x44494 <_free>
   75a3c: aa1503e0     	mov	x0, x21
   75a40: 9467c72a     	bl	0x1a676e8 <domain_curr_field+0x1a676a6>
   75a44: aa0003f5     	mov	x21, x0
   75a48: aa1303e0     	mov	x0, x19
   75a4c: 97ff3a92     	bl	0x44494 <_free>
   75a50: aa1503e0     	mov	x0, x21
   75a54: 9467c725     	bl	0x1a676e8 <domain_curr_field+0x1a676a6>

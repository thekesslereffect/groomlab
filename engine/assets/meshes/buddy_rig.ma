//Maya ASCII 2022 scene
// SPDX-FileCopyrightText: 2026 Nick Rutlinh
// SPDX-License-Identifier: CC-BY-4.0
// Buddy: a posable cartoon character rig to groom (centimetres, y up, facing +z; body, eyes, teeth,
// tongue skinned to one joint hierarchy, posed with the *_CTL controls).
// Credit: "Buddy by Nick Rutlinh (GroomLab), CC BY 4.0".
//Name: buddy_rig.ma
//Last modified: Tue, Oct 20, 2026 01:44:56 PM
//Codeset: 1252
requires maya "2022";
currentUnit -l centimeter -a degree;
fileInfo "application" "maya";
fileInfo "product" "Maya 2022";
fileInfo "version" "2022";
fileInfo "cutIdentifier" "202303271415-baa69b5798";
fileInfo "osv" "Windows 10 Enterprise v2009 (Build: 26200)";
fileInfo "UUID" "E08B174F-4974-D9B6-67C5-DDA4FA533974";
createNode transform -n "buddy_rig";
	rename -uid "098552EB-4D11-846C-B48F-89A1FAE09D48";
createNode transform -n "main_CTL" -p "buddy_rig";
	rename -uid "DE6685B6-4E8B-003A-7493-9B959E28DD5E";
	addAttr -ci true -k true -sn "globalScale" -ln "globalScale" -dv 1 -min 0.01 -at "double";
	setAttr -l on -k off ".v";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr -k on ".globalScale";
createNode nurbsCurve -n "main_CTLShape" -p "main_CTL";
	rename -uid "BC54410B-49BD-89A2-A5A6-678DCFE1C7E7";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 17;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		1 16 0 no 3
		17 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16
		17
		34.5 0 0
		27.716385975338603 0 11.480502970952694
		21.213203435596427 0 21.213203435596427
		11.480502970952696 0 27.716385975338603
		2.1125157285291845e-15 0 34.5
		-11.480502970952692 0 27.716385975338603
		-21.213203435596423 0 21.213203435596427
		-27.716385975338603 0 11.480502970952697
		-34.5 0 4.2250314570583689e-15
		-27.716385975338607 0 -11.48050297095269
		-21.21320343559643 0 -21.213203435596423
		-11.48050297095271 0 -27.716385975338596
		-6.3375471855875526e-15 0 -34.5
		11.480502970952701 0 -27.7163859753386
		21.213203435596419 0 -21.21320343559643
		27.716385975338596 0 -11.480502970952712
		34.5 0 -8.4500629141167378e-15
		;
	setAttr ".adot" yes;
createNode transform -n "skeleton_GRP" -p "main_CTL";
	rename -uid "8E6A5928-4FC4-C099-ED0B-33AA3410A858";
createNode joint -n "root" -p "skeleton_GRP";
	rename -uid "8A7A8A15-4DEB-961F-C911-B396701BFBC1";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 90 0 90 ;
	setAttr ".bps" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 11 1 1;
	setAttr ".radi" 1.2;
createNode joint -n "hips" -p "root";
	rename -uid "144C5DD8-4918-399F-EC12-3A94E7A95FFA";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".bps" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 11 1 1;
	setAttr ".radi" 1.2;
createNode joint -n "spine" -p "hips";
	rename -uid "A75C5356-4D9B-A44B-60B8-E0815F63D39D";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".bps" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 19 1 1;
	setAttr ".radi" 1.2;
createNode joint -n "head" -p "spine";
	rename -uid "EDC845BF-4FB0-FBDD-AF6A-F3AC00623D36";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".bps" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 28 1 1;
	setAttr ".radi" 1.2;
createNode joint -n "headEnd" -p "head";
	rename -uid "ED140776-4844-C511-4564-9C9CF83F0CA4";
	setAttr ".t" -type "double3" 29 -1 0 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".radi" 1.2;
createNode joint -n "upShoulder_L" -p "head";
	rename -uid "5EF227DF-47BF-2FB4-0E52-2FB04C6AA67A";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 49.289957417995119 -84.982002891423051 -139.39870535499563 ;
	setAttr ".bps" -type "matrix" 0.99616727256193893 -0.066411151504129906 -0.056923844146396674 0
		 0.06630298994781092 0.99779234260235516 -0.0037887422827322936 0 0.057049790538513001 1.1102230246251565e-16 0.99837133442397663 0
		 16 35.5 -4.7999999999999998 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "upElbow_L" -p "upShoulder_L";
	rename -uid "48D532ED-488D-C7AE-0957-8D8B88B0A92D";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -0.078774276731399376 -3.8543147501225046 4.9796103462861536 ;
	setAttr ".bps" -type "matrix" 0.99973979339155139 0.020402852926358926 0.010201426463178911 0
		 -0.020401790803298136 0.99979184013096722 -0.0002081815388014596 0 -0.010203550432900665 -7.8091089426893024e-15 0.99994794242428597 0
		 26.5 34.799999999999997 -5.4000000000000004 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "upWrist_L" -p "upElbow_L";
	rename -uid "B497683E-4063-6C30-06E4-3BA16EB1C566";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.038334688053347103 2.8749852142838801 -0.40482835539985512 ;
	setAttr ".bps" -type "matrix" 0.99911229454319339 0.013321497260576767 -0.039964491781727132 0
		 -0.013310852834388423 0.99991126491841098 0.00053243411311751257 0 0.039968038348867418 2.5767130995536792e-13 0.99920095872178949 0
		 36.299999999999997 35 -5.2999999999999998 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "upThumb1_L" -p "upWrist_L";
	rename -uid "98FE9148-4AD1-0808-B2BA-F48B1C75E1AA";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 87.918076111310569 -18.82991560731832 59.238179780174498 ;
	setAttr ".bps" -type "matrix" 0.4857413925751331 0.81968859997061549 0.30358837035924557 0
		 -0.15477007687694022 -0.26117450472986187 0.95280328577446516 0 0.86029153363419564 -0.50980239030169694 -1.1102230246251565e-16 0
		 38.299999999999997 38.499999999999979 -4.599999999999973 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "upThumb2_L" -p "upThumb1_L";
	rename -uid "1F99AF4D-43C1-27B8-E906-A4B875B2D04B";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 1.3105531753095558 8.7524400588601985 -9.1231090892213889 ;
	setAttr ".bps" -type "matrix" 0.36735917918532535 0.91839794796334295 0.14694367167389305 0
		 -0.054573509608920788 -0.13643377402230719 0.98914486166334337 0 0.92847669088526341 -0.37139067635409362 -3.3174535239836786e-16 0
		 39.899999999999999 41.200000000000252 -3.6000000000006702 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "upThumbEnd_L" -p "upThumb2_L";
	rename -uid "AD9E9C22-44B4-17DE-CD1D-2798155F1B9E";
	setAttr ".t" -type "double3" 2.7221315177632395 -2.886579864025407e-15 -8.3899376624717391e-15 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "upThumb2_L_parentConstraint" -p "upThumb2_L";
	rename -uid "DB17672B-48A9-6D73-1087-9E8480D5A5B7";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upThumb2_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 2.8421709430404007e-14 0 0 ;
	setAttr ".tg[0].tor" -type "double3" -3.5781240166464568e-15 6.3611093629270335e-15 
		-2.0126947593636317e-15 ;
	setAttr ".lr" -type "double3" 4.075085685625131e-15 4.2241741863187315e-16 5.0938571070314139e-15 ;
	setAttr ".rst" -type "double3" 3.2939338184001485 -3.5527136788005009e-15 -5.3290705182007514e-15 ;
	setAttr ".rsrr" -type "double3" 3.0811623476677818e-15 -4.5223511877059377e-15 1.8636062586700292e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upThumb1_L_parentConstraint" -p "upThumb1_L";
	rename -uid "165DCB16-4A78-376A-C0C9-B49022BBD86F";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upThumb1_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 7.1054273576010019e-15 -7.1054273576010019e-15 
		-1.7763568394002505e-15 ;
	setAttr ".tg[0].tor" -type "double3" 1.9083328088781104e-14 3.0414054141494884e-14 
		1.272221872585407e-14 ;
	setAttr ".lr" -type "double3" -1.9083328088781097e-14 -2.5444437451708134e-14 -1.5902773407317578e-14 ;
	setAttr ".rst" -type "double3" 2.0168746852511958 3.4734404254248119 0.77937674780391264 ;
	setAttr ".rsrr" -type "double3" -1.9083328088781097e-14 -2.5444437451708134e-14 
		-1.5902773407317578e-14 ;
	setAttr -k on ".w0";
createNode joint -n "upFingerA1_L" -p "upWrist_L";
	rename -uid "80E00829-4997-9751-A3AA-89B8FBE478A4";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -0.01721826932909374 0.1981963713834321 -5.7283914656160295 ;
	setAttr ".bps" -type "matrix" 0.99530734153820566 -0.086548464481594756 -0.043274232240768859 0
		 0.086466776436558229 0.99624764155097412 -0.003759425061376615 0 0.043437224276190747 -1.0755126413435614e-12 0.99905615835506456 0
		 43.799999999999997 37.200000000000003 -5.5999999999999828 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "upFingerA2_L" -p "upFingerA1_L";
	rename -uid "8591CA84-477D-3F4B-141B-A5852C2EE2C7";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.23159184795069926 -5.0868598028178891 2.3549046704374521 ;
	setAttr ".bps" -type "matrix" 0.99794026557833193 -0.045360921162700414 0.045360921162602721 0
		 0.045314133196497505 0.99897066364897391 0.0020597333271061766 0 -0.045407660918601211 5.0195924797903377e-15 0.99896854021030179 0
		 46.100000000000001 36.999999999999979 -5.6999999999999309 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "upFingerAEnd_L" -p "upFingerA2_L";
	rename -uid "81B662DC-4AD4-1E39-9A14-6797F88E5F6E";
	setAttr ".t" -type "double3" 2.204540768504863 -1.0907073855204175e-15 1.8041124150158794e-16 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "upFingerA2_L_parentConstraint" -p "upFingerA2_L";
	rename -uid "282BC1F5-4DFE-9FFA-B759-969B78C75A16";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upFingerA2_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 7.1054273576010019e-15 0 -3.5527136788005009e-15 ;
	setAttr ".tg[0].tor" -type "double3" 3.4787316828507215e-16 -2.3854160110976376e-15 
		1.1864959846865854e-15 ;
	setAttr ".lr" -type "double3" -1.2424041724466862e-16 1.5933833511628751e-15 3.1060104311167141e-18 ;
	setAttr ".rst" -type "double3" 2.3108440016582819 1.4210854715202004e-14 -8.8817841970012523e-16 ;
	setAttr ".rsrr" -type "double3" -2.8575295966273782e-16 1.5918303459473166e-15 -1.1833899742554686e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upFingerA1_L_parentConstraint" -p "upFingerA1_L";
	rename -uid "A47818E7-4D18-C540-3ABD-C9ADD42E3CF8";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upFingerA1_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 1.4210854715202004e-14 0 ;
	setAttr ".tg[0].tor" -type "double3" -1.0871036508908507e-16 3.478731682850722e-16 
		3.9048617545760399e-15 ;
	setAttr ".lr" -type "double3" 8.5415286855709677e-17 -3.445730322020106e-16 -3.9756114550699811e-15 ;
	setAttr ".rst" -type "double3" 7.5346388505817288 2.0998136563286707 5.5333515547317802e-13 ;
	setAttr ".rsrr" -type "double3" 1.1181637552020177e-16 -3.1831753777647717e-16 -4.7707000774162178e-15 ;
	setAttr -k on ".w0";
createNode joint -n "upFingerB1_L" -p "upWrist_L";
	rename -uid "E73CB266-4866-0740-114B-809498DAE897";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -0.017218269329093958 0.19819637138343424 -5.7283914656160917 ;
	setAttr ".bps" -type "matrix" 0.99530734153820566 -0.086548464481595852 -0.043274232240768894 0
		 0.086466776436559312 0.99624764155097401 -0.0037594250613766627 0 0.043437224276190782 -1.0755159346076603e-12 0.99905615835506456 0
		 43.800000000000011 33 -5.6000000000000156 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "upFingerB2_L" -p "upFingerB1_L";
	rename -uid "90C33157-4625-2184-0D66-36B771D9622E";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -0.37790151394615046 -7.9209328221010091 7.7052857247318816 ;
	setAttr ".bps" -type "matrix" 0.99437882000179501 0.047351372380932025 0.094702744762126062 0
		 -0.047138076826326515 0.99887829465538103 -0.0044893406501450919 0 -0.094809092628046349 1.6188213384121711e-14 0.99549547259394733 0
		 46.100000000000009 32.799999999999976 -5.6999999999999629 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "upFingerBEnd_L" -p "upFingerB2_L";
	rename -uid "EB5D904B-4EAA-544B-B55B-269E73B0836C";
	setAttr ".t" -type "double3" 2.1118712081942963 9.1849271244281994e-15 5.8286708792820718e-16 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "upFingerB2_L_parentConstraint" -p "upFingerB2_L";
	rename -uid "5605C9CB-47EE-C25E-9BD4-4CB5363EF515";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upFingerB2_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 7.1054273576010019e-15 3.5527136788005009e-15 
		-1.7763568394002505e-15 ;
	setAttr ".tg[0].tor" -type "double3" -6.9574633657014429e-16 4.7708320221952752e-15 
		-3.4725196619884879e-15 ;
	setAttr ".lr" -type "double3" 7.9513867036587899e-16 -1.6259964606896004e-15 4.7289008813751982e-15 ;
	setAttr ".rst" -type "double3" 2.3108440016582747 1.4210854715202004e-14 8.8817841970012523e-16 ;
	setAttr ".rsrr" -type "double3" 5.963540027744092e-16 -1.6228904502584836e-15 3.144835561505674e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upFingerB1_L_parentConstraint" -p "upFingerB1_L";
	rename -uid "CAA6AF83-43B5-E5D5-D056-4DB26444AE45";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upFingerB1_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 1.4210854715202004e-14 7.1054273576010019e-15 
		1.7763568394002505e-15 ;
	setAttr ".tg[0].tor" -type "double3" -7.4544250346801187e-17 1.3169484227934873e-15 
		3.1112154251594853e-15 ;
	setAttr ".lr" -type "double3" 5.1249172113425827e-17 -1.3177734568142528e-15 -1.5903941194442564e-15 ;
	setAttr ".rst" -type "double3" 7.4786885620873491 -2.0998136563286636 -5.6088467204062908e-13 ;
	setAttr ".rsrr" -type "double3" 7.9203265993476277e-17 -1.3164145772506393e-15 -3.180628995189652e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upWrist_L_parentConstraint" -p "upWrist_L";
	rename -uid "D2484B90-48B3-CFFD-6C34-87AEF901BA2A";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upWrist_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 7.1054273576010019e-15 -7.1054273576010019e-15 
		0 ;
	setAttr ".tg[0].tor" -type "double3" 0 3.975693351829396e-16 2.0305543193425529e-16 ;
	setAttr ".lr" -type "double3" -3.7272125173400593e-17 -7.9494454471393441e-16 6.4566191836838723e-16 ;
	setAttr ".rst" -type "double3" 9.8025506884687914 0 1.7763568394002505e-15 ;
	setAttr ".rsrr" -type "double3" 5.1734935645686115e-34 -3.9764213230241889e-16 -1.4908850069360235e-16 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upElbow_L_parentConstraint" -p "upElbow_L";
	rename -uid "77B6377B-4EF1-B7FE-7708-2C843BF69CDB";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upElbow_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -3.5527136788005009e-15 -7.1054273576010019e-15 
		-8.8817841970012523e-16 ;
	setAttr ".tg[0].tor" -type "double3" 1.2424041724466862e-17 1.5902773407317584e-15 
		-1.5700882729294998e-15 ;
	setAttr ".lr" -type "double3" -3.3077325318600718e-32 -1.5879478329084208e-15 2.3869690163131958e-15 ;
	setAttr ".rst" -type "double3" 10.540398474441089 -7.1054273576010019e-15 0 ;
	setAttr ".rsrr" -type "double3" -2.2069510443105436e-32 -1.5887243355162001e-15 
		1.5918303459473166e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upShoulder_L_parentConstraint" -p "upShoulder_L";
	rename -uid "20D83C5C-4A55-1AA2-7F2A-37AF71804424";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upShoulder_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -1.7763568394002505e-15 -7.1054273576010019e-15 
		-4.4408920985006262e-16 ;
	setAttr ".tg[0].tor" -type "double3" 1.3169484227934875e-14 3.5781240166464568e-15 
		-1.2722218725854067e-14 ;
	setAttr ".lr" -type "double3" -2.5444437451708134e-14 7.0622500768802538e-31 3.1805546814635168e-15 ;
	setAttr ".rst" -type "double3" 7.4999999999999929 -5.8 15.999999999999998 ;
	setAttr ".rsrr" -type "double3" -2.5444437451708134e-14 7.0622500768802538e-31 3.1805546814635168e-15 ;
	setAttr -k on ".w0";
createNode joint -n "upShoulder_R" -p "head";
	rename -uid "634A404C-4D16-E783-D713-84835BA1FBA7";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 49.289957417995041 -84.982002891423036 40.601294645004387 ;
	setAttr ".bps" -type "matrix" 0.99616727256193893 0.066411151504129795 0.05692384414639684 0
		 0.06630298994781092 -0.99779234260235516 0.0037887422827336259 0 0.057049790538513223 -1.6653345369377348e-15 -0.99837133442397663 0
		 -16 35.5 -4.7999999999999998 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "upElbow_R" -p "upShoulder_R";
	rename -uid "40F907B0-41A7-26D7-2ACD-FF942C761817";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -0.078774276723198658 -3.8543147501225175 4.979610346286143 ;
	setAttr ".bps" -type "matrix" 0.9997397933915515 -0.020402852926358961 -0.010201426463178849 0
		 -0.020401790803299413 -0.99979184013096734 0.00020818153865966114 0 -0.010203550432897751 1.4935652062139477e-13 -0.99994794242428608 0
		 -26.5 34.800000000000004 -5.4000000000000004 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "upWrist_R" -p "upElbow_R";
	rename -uid "EBEFDAD2-46CA-7C31-A206-008A1EF3CF63";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.038334688090915711 2.8749852142838224 -0.4048283554002633 ;
	setAttr ".bps" -type "matrix" 0.9991122945431935 -0.013321497260576786 0.039964491781727188 0
		 -0.013310852834356367 -0.99991126491841098 -0.00053243411391455198 0 0.039968038348878063 5.3970527797101044e-13 -0.99920095872178916 0
		 -36.299999999999997 35.000000000000007 -5.2999999999999998 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "upThumb1_R" -p "upWrist_R";
	rename -uid "BB3AEC50-442B-D966-E841-8880C5D9B0DE";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 87.918076111285885 -18.829915607279059 59.238179780182463 ;
	setAttr ".bps" -type "matrix" 0.48574139257513332 -0.81968859997061572 -0.3035883703592458 0
		 -0.1547700768769405 0.26117450472986181 -0.95280328577446538 0 0.86029153363419575 0.50980239030169694 1.457167719820518e-16 0
		 -38.300000000000004 38.500000000000007 -4.5999999999999988 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "upThumb2_R" -p "upThumb1_R";
	rename -uid "4C8EF712-4898-9BAD-1FEB-EB82303C86F0";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 1.3105531753095554 8.7524400588601985 -9.1231090892213853 ;
	setAttr ".bps" -type "matrix" 0.36735917918532557 -0.91839794796334295 -0.14694367167389333 0
		 -0.054573509608921073 0.13643377402230716 -0.98914486166334359 0 0.92847669088526352 0.37139067635409351 3.1051568475252773e-16 0
		 -39.900000000000006 41.20000000000001 -3.5999999999999979 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "upThumbEnd_R" -p "upThumb2_R";
	rename -uid "7AB704AB-41DF-B3E4-A17C-C29164835118";
	setAttr ".t" -type "double3" -2.7221315177632306 -6.4659388954169117e-13 -3.5527136788005009e-14 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".jo" -type "double3" 1.2074182697257331e-06 -2.0107553368741429e-22 -7.810522178558392e-23 ;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "upThumb2_R_parentConstraint" -p "upThumb2_R";
	rename -uid "DA418F58-4EFA-B38F-A50A-4281B5C3F177";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upThumb2_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 3.5527136788005009e-15 7.1054273576010019e-15 ;
	setAttr ".tg[0].tor" -type "double3" 0 7.9513867036587919e-15 5.2429456077250156e-15 ;
	setAttr ".lr" -type "double3" -1.2523434058262595e-14 -1.2622826392058334e-14 -1.8412429835659891e-14 ;
	setAttr ".rst" -type "double3" -3.2939338184001343 -7.3541173151170369e-13 -1.3677947663381929e-13 ;
	setAttr ".rsrr" -type "double3" -6.95746336570144e-16 -6.5847421139674373e-15 -4.7708320221952752e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upThumb1_R_parentConstraint" -p "upThumb1_R";
	rename -uid "12FC65B1-405F-BC1F-751F-40800BBA4E5C";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upThumb1_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 1.4210854715202004e-14 -3.5527136788005009e-15 
		-1.2434497875801753e-14 ;
	setAttr ".tg[0].tor" -type "double3" 8.7465253740246703e-15 1.5306419404543174e-14 
		-3.1805546814635168e-15 ;
	setAttr ".lr" -type "double3" -3.1805546814635164e-15 -1.272221872585407e-14 -9.5416640443905519e-15 ;
	setAttr ".rst" -type "double3" -2.0168746852512029 -3.4734404254254585 -0.77937674780111976 ;
	setAttr ".rsrr" -type "double3" 0 -1.2722218725854067e-14 0 ;
	setAttr -k on ".w0";
createNode joint -n "upFingerA1_R" -p "upWrist_R";
	rename -uid "BC5F66F9-4290-198E-25B9-41AA2458E829";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -0.01721826962833433 0.19819637137887161 -5.7283914656161867 ;
	setAttr ".bps" -type "matrix" 0.99530734153820577 0.0865484644815947 0.043274232240768935 0
		 0.086466776436365994 -0.99624764155097412 0.0037594250658014748 0 0.04343722427657376 -3.3371310002317674e-12 -0.99905615835504802 0
		 -43.799999999999997 37.20000000000001 -5.5999999999999988 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "upFingerA2_R" -p "upFingerA1_R";
	rename -uid "8CFF7756-4B93-D6EF-A85B-90BFDCF6AD8C";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.23159184820840464 -5.0868598028283198 2.3549046704148751 ;
	setAttr ".bps" -type "matrix" 0.99794026557833204 0.045360921162700449 -0.045360921162602721 0
		 0.045314133196495264 -0.99897066364897391 -0.0020597333271611313 0 -0.045407660918603723 4.9705620182133471e-14 -0.99896854021030168 0
		 -46.100000000000009 37 -5.7000000000000011 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "upFingerAEnd_R" -p "upFingerA2_R";
	rename -uid "7AD7F270-4799-8E57-C1E9-E28CCC12C9F3";
	setAttr ".t" -type "double3" -2.2045407685048488 -9.9475983006414026e-14 -1.0835776720341528e-13 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".jo" -type "double3" 1.2074182697257331e-06 -3.2514410500042476e-23 -8.3781473055256043e-23 ;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "upFingerA2_R_parentConstraint" -p "upFingerA2_R";
	rename -uid "CAF99EE0-44BA-11B6-2392-C3AA2C6F5A2B";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upFingerA2_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 1.4210854715202004e-14 7.1054273576010019e-15 
		-8.8817841970012523e-16 ;
	setAttr ".tg[0].tor" -type "double3" -9.9392333795734924e-17 7.9513867036587919e-16 
		4.2552342906299001e-16 ;
	setAttr ".lr" -type "double3" -1.2535858099987062e-14 3.4243765003061775e-15 -3.82971086156691e-15 ;
	setAttr ".rst" -type "double3" -2.3108440016582747 -2.1316282072803006e-14 5.5067062021407764e-14 ;
	setAttr ".rsrr" -type "double3" 1.6151254241806922e-16 -1.5949363563784332e-15 -4.0378135604517293e-16 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upFingerA1_R_parentConstraint" -p "upFingerA1_R";
	rename -uid "B30FB4DF-421D-62F7-EE9A-1497A21C895F";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upFingerA1_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 7.1054273576010019e-15 1.4210854715202004e-14 
		-4.4408920985006262e-16 ;
	setAttr ".tg[0].tor" -type "double3" -6.2120208622334327e-18 -2.4848083448933725e-17 
		-3.1833937691232095e-15 ;
	setAttr ".lr" -type "double3" -1.2411617682742396e-14 4.7845178806573833e-15 -2.4061722897891728e-15 ;
	setAttr ".rst" -type "double3" -7.5346388505817217 -2.0998136563286636 1.1359801987964602e-12 ;
	setAttr ".rsrr" -type "double3" 1.0871036508908506e-17 5.0424138092660427e-17 3.1805895634166007e-15 ;
	setAttr -k on ".w0";
createNode joint -n "upFingerB1_R" -p "upWrist_R";
	rename -uid "57CB5D36-4BCC-5557-A41F-69830095C090";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -0.017218269678245947 0.19819637137887355 -5.7283914656162462 ;
	setAttr ".bps" -type "matrix" 0.99530734153820566 0.086548464481595741 0.043274232240768963 0
		 0.086466776436329204 -0.99624764155097412 0.0037594250666718159 0 0.043437224276649117 -4.2049777289569784e-12 -0.99905615835504469 0
		 -43.799999999999997 33.000000000000007 -5.5999999999999988 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "upFingerB2_R" -p "upFingerB1_R";
	rename -uid "F6F50E04-45CC-D9E5-FBE4-AB847B040975";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -0.37790151362899393 -7.920932822141725 7.7052857246900066 ;
	setAttr ".bps" -type "matrix" 0.99437882000179501 -0.047351372380932046 -0.094702744762126034 0
		 -0.047138076826348345 -0.99887829465538114 0.004489340649913607 0 -0.09480909262803533 2.1586479650511283e-13 -0.99549547259394833 0
		 -46.099999999999994 32.79999999999999 -5.7000000000000002 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "upFingerBEnd_R" -p "upFingerB2_R";
	rename -uid "76CC39E6-4213-329C-58F6-4CB3BAA6489E";
	setAttr ".t" -type "double3" -2.1118712081942945 -2.2737367544323206e-13 1.0658141036401503e-13 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".jo" -type "double3" 1.2074182697257333e-06 -1.7672650869201902e-24 -3.3512589103685584e-23 ;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "upFingerB2_R_parentConstraint" -p "upFingerB2_R";
	rename -uid "C44BD2FB-476B-DF1A-4C0A-77AED9910B4F";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upFingerB2_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -1.4210854715202004e-14 1.0658141036401503e-14 
		0 ;
	setAttr ".tg[0].tor" -type "double3" 7.4544250346801179e-16 4.7708320221952752e-15 
		4.7087118135729408e-15 ;
	setAttr ".lr" -type "double3" -1.2821611059649799e-14 5.6001368073034361e-15 -7.089468809023902e-15 ;
	setAttr ".rst" -type "double3" -2.3108440016582605 -7.1054273576010019e-15 4.9737991503207013e-14 ;
	setAttr ".rsrr" -type "double3" -7.951386703658788e-16 -3.1696836449546074e-15 -4.7490899491774563e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upFingerB1_R_parentConstraint" -p "upFingerB1_R";
	rename -uid "58E6BB2D-4DD3-C024-23B1-0489DCFC9E69";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upFingerB1_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 2.1316282072803006e-14 0 ;
	setAttr ".tg[0].tor" -type "double3" -3.7272125173400593e-17 3.7272125173400585e-16 
		-3.8712416182298496e-15 ;
	setAttr ".lr" -type "double3" -1.2371239547137878e-14 4.3631196216982202e-15 -8.1588433281082388e-16 ;
	setAttr ".rst" -type "double3" -7.4786885620873065 2.0998136563286707 -1.1306511282782594e-12 ;
	setAttr ".rsrr" -type "double3" 3.5719119957842215e-17 -3.9659870692321563e-16 4.7708502214751452e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upWrist_R_parentConstraint" -p "upWrist_R";
	rename -uid "30DB09FF-4076-A72B-37F0-D097BC15D9E6";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upWrist_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 7.1054273576010019e-15 0 -2.6645352591003757e-15 ;
	setAttr ".tg[0].tor" -type "double3" -1.8636062586700297e-17 1.590277340731758e-15 
		-1.6772456328030259e-16 ;
	setAttr ".lr" -type "double3" -1.2026472389283922e-14 4.3281284729351699e-15 -5.4252295949011787e-15 ;
	setAttr ".rst" -type "double3" -9.8025506884687879 7.1054273576010019e-15 -3.5527136788005009e-15 ;
	setAttr ".rsrr" -type "double3" 2.3295078233375365e-17 -2.3852946825651726e-15 1.0094533901129326e-16 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upElbow_R_parentConstraint" -p "upElbow_R";
	rename -uid "92E6EEEF-40BF-43C8-CBA6-D39836B58B5E";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upElbow_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -1.4210854715202004e-14 -2.1316282072803006e-14 
		8.8817841970012523e-16 ;
	setAttr ".tg[0].tor" -type "double3" -4.9696166897867449e-17 0 -2.6191432960391704e-15 ;
	setAttr ".lr" -type "double3" -1.2001624305834988e-14 5.2957477850540004e-15 -1.9812464037485758e-15 ;
	setAttr ".rst" -type "double3" -10.540398474441092 -2.8421709430404007e-14 -3.1086244689504383e-15 ;
	setAttr ".rsrr" -type "double3" -1.6498225831282509e-32 -7.9203265993476247e-16 
		2.3869690163131958e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "upShoulder_R_parentConstraint" -p "upShoulder_R";
	rename -uid "AF3AE227-4FFA-40EA-6E3D-BDAD944433DA";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "upShoulder_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 -7.1054273576010019e-15 -4.4408920985006262e-16 ;
	setAttr ".tg[0].tor" -type "double3" -2.1665043959125319e-13 -9.5416640443905535e-15 
		-7.1562480332929135e-15 ;
	setAttr ".lr" -type "double3" 1.9083328088781099e-13 1.59027734073176e-14 -9.5416640443905235e-15 ;
	setAttr ".rst" -type "double3" 7.5000000000000071 -5.8 -16 ;
	setAttr ".rsrr" -type "double3" 2.0991660897659211e-13 1.2722218725854073e-14 -3.1805546814634935e-15 ;
	setAttr -k on ".w0";
createNode joint -n "eye_L" -p "head";
	rename -uid "3DB78068-4A6E-1452-809E-EBBD3B568412";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".t" -type "double3" 10.174 8.4616 7.61 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".jo" -type "double3" 0 -75 -90 ;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode aimConstraint -n "eye_L_aimConstraint" -p "eye_L";
	rename -uid "281B6DDE-4FFD-A71B-6268-ECBBB174A0CD";
	addAttr -dcb 0 -ci true -sn "w0" -ln "eye_L_CTLW0" -dv 1 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".a" -type "double3" 0 0 1 ;
	setAttr ".wut" 2;
	setAttr ".rsrr" -type "double3" 1.2722218725854061e-14 1.2722218725854067e-14 -2.8624992133171654e-14 ;
	setAttr -k on ".w0";
createNode joint -n "eye_R" -p "head";
	rename -uid "29A0CCEE-4C17-7057-1178-58AFC23A9EB3";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".t" -type "double3" 10.174 8.4616 -7.61 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 180 -75 90 ;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode aimConstraint -n "eye_R_aimConstraint" -p "eye_R";
	rename -uid "3D673CB7-41B8-7528-26ED-9F87F9C04ED6";
	addAttr -dcb 0 -ci true -sn "w0" -ln "eye_R_CTLW0" -dv 1 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".a" -type "double3" 0 0 1 ;
	setAttr ".wut" 2;
	setAttr ".rsrr" -type "double3" 1.9083328088781097e-14 -1.2722218725854064e-14 -6.3611093629270335e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "head_parentConstraint" -p "head";
	rename -uid "BA56824D-43B9-8C08-865E-C199818042D9";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "head_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".rst" -type "double3" 9 0 0 ;
	setAttr -k on ".w0";
createNode joint -n "loShoulder_L" -p "spine";
	rename -uid "5D52F48C-45D0-7636-A478-1DB523126397";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 49.207034055317706 -83.555512044930083 -139.38659336288913 ;
	setAttr ".bps" -type "matrix" 0.99368106829059422 -0.085203913285456512 -0.073063175962368526 0
		 0.084974522483502943 0.99636353464026595 -0.0062479891050062353 0 0.073329837375820661 1.110223024625157e-16 0.99730774335228933 0
		 16.5 19.5 -1 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "loElbow_L" -p "loShoulder_L";
	rename -uid "28556EDD-4314-49AE-873E-E18F34C0FCC2";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.097090872592887068 -4.2245823943083707 3.5699958052975274 ;
	setAttr ".bps" -type "matrix" 0.99973690272738103 -0.022934651399784026 0.00035649717201706455 0
		 0.022934649941632055 0.9997369662892186 8.1782895103219271e-06 0 -0.00035659096746198937 2.0661122149954996e-14 0.99999993642143903 0
		 25.585000000000001 18.721 -1.6679999999999997 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "loWrist_L" -p "loElbow_L";
	rename -uid "EE998501-4EB6-EEAF-1553-0395974E01F0";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.33378093321853969 -0.8897968796235538 -19.248887884355735 ;
	setAttr ".bps" -type "matrix" 0.93616767052418659 -0.35119575027525279 0.015863090932106937 0
		 0.35114534289864135 0.93630205862670302 0.0059500564697729111 0 -0.01694227924199633 -7.7310658411994563e-15 0.9998564692865104 0
		 33.997999999999998 18.527999999999999 -1.6649999999999998 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "loThumb1_L" -p "loWrist_L";
	rename -uid "00343D0F-46A1-81DD-F34F-A29309488CEB";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 90.868851202962716 -12.887106561821961 81.111154518829252 ;
	setAttr ".bps" -type "matrix" 0.47542194344606004 0.84885696101420904 0.23111866092484337 0
		 -0.11293657638623215 -0.20164613842533546 0.97292556990363221 0 0.87247882805494381 -0.4886519155757722 -3.6429192995512949e-17 0
		 35.663000000000011 21.808000000000018 -0.66100000000006776 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "loThumb2_L" -p "loThumb1_L";
	rename -uid "CB66CCAF-40FD-C2A9-6688-BBB514324D1B";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.92887510270754581 16.780262712054881 -10.148617095669943 ;
	setAttr ".bps" -type "matrix" 0.21522222383529793 0.97508844268237926 0.053683547895432684 0
		 -0.0115705773494802 -0.052421799420097476 0.9985579986587455 0 0.97649655202012309 -0.21553302274317798 -6.316182941711905e-16 0
		 36.88900000000001 23.997000000000014 -0.065000000000050906 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "loThumbEnd_L" -p "loThumb2_L";
	rename -uid "5CBA29AD-438D-F315-BDD8-80806DE8FBF7";
	setAttr ".t" -type "double3" 2.0490448994592612 -1.3877787807814457e-17 -9.812616648214007e-16 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "loThumb2_L_parentConstraint" -p "loThumb2_L";
	rename -uid "DEDA23CB-4CE7-8F69-390C-028DF91FCAF7";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loThumb2_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -7.1054273576010019e-15 -3.1086244689504383e-15 
		0 ;
	setAttr ".tg[0].tor" -type "double3" -8.9453100416161419e-16 -1.5902773407317587e-14 
		-1.6399735076296263e-15 ;
	setAttr ".lr" -type "double3" 9.9392333795734938e-16 6.6841344477631723e-15 6.609590197416371e-15 ;
	setAttr ".rst" -type "double3" 2.5787619122361889 1.7763568394002505e-15 0 ;
	setAttr ".rsrr" -type "double3" 5.9635400277440989e-16 1.3020395727241272e-14 4.8702243559910101e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loThumb1_L_parentConstraint" -p "loThumb1_L";
	rename -uid "83852951-4656-C596-FA89-F4804D4BFEFE";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loThumb1_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -7.1054273576010019e-15 0 -7.1054273576010019e-15 ;
	setAttr ".tg[0].tor" -type "double3" -2.2164490436448882e-14 3.5532759331975222e-15 
		-7.9513867036587903e-15 ;
	setAttr ".lr" -type "double3" 2.2263882770244617e-14 -6.3611093629270335e-15 -1.2358937634540444e-30 ;
	setAttr ".rst" -type "double3" 0.42272365381578325 3.6617016049174929 0.97564700022563722 ;
	setAttr ".rsrr" -type "double3" 2.2263882770244617e-14 -6.3611093629270335e-15 -1.2358937634540444e-30 ;
	setAttr -k on ".w0";
createNode joint -n "loFingerA1_L" -p "loWrist_L";
	rename -uid "A1FE251B-40BA-47E2-F981-40AF77139344";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.10759895187179151 15.754415943656387 20.956755684056045 ;
	setAttr ".bps" -type "matrix" 0.96687323370463074 0.0066566143456458955 -0.255170216582978 0
		 -0.006436244836137273 0.99997784449729332 0.001698607357794101 0 0.25517587013265935 -6.6634738033411662e-15 0.9668946557417929 0
		 39.785000000000011 18.233000000000008 -1.652999999999994 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "loFingerA2_L" -p "loFingerA1_L";
	rename -uid "91E3496B-45A0-64CA-3696-EE9D67A2A685";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.51982383854158365 6.8774812014232642 3.9514721757773077 ;
	setAttr ".bps" -type "matrix" 0.92663734741845638 0.075007177835449523 -0.3683980858289686 0
		 -0.069700799886994041 0.99718299387482578 0.027710561559664029 0 0.36943879718351191 -6.3129788246920765e-15 0.92925506462736052 0
		 41.528000000000006 18.245000000000008 -2.1129999999999933 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "loFingerAEnd_L" -p "loFingerA2_L";
	rename -uid "72B7841B-4A1D-3FDA-0098-7BAD7B3550F7";
	setAttr ".t" -type "double3" 1.5065224857266495 1.1848161340921592e-15 -2.4424906541753444e-15 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "loFingerA2_L_parentConstraint" -p "loFingerA2_L";
	rename -uid "BC7CA19A-450E-26FE-8E89-89AE489EAC22";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loFingerA2_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -2.1316282072803006e-14 7.1054273576010019e-15 
		-5.3290705182007514e-15 ;
	setAttr ".tg[0].tor" -type "double3" 1.9878466759146985e-16 3.1805546814635168e-15 
		9.8149929623288212e-16 ;
	setAttr ".lr" -type "double3" 6.9574633657014429e-16 -2.6401088664492056e-17 -4.7459839387463414e-15 ;
	setAttr ".rst" -type "double3" 1.8027182253474763 -7.1054273576010019e-15 0 ;
	setAttr ".rsrr" -type "double3" -1.987846675914698e-16 -2.3823100006665211e-15 4.132649085210537e-33 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loFingerA1_L_parentConstraint" -p "loFingerA1_L";
	rename -uid "04271D46-4015-6A8A-1570-D9A2E9B3996F";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loFingerA1_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -1.4210854715202004e-14 -3.5527136788005009e-15 
		-3.5527136788005009e-15 ;
	setAttr ".tg[0].tor" -type "double3" 3.4414595576773215e-15 -9.5416640443905503e-15 
		-1.2840247122236503e-14 ;
	setAttr ".lr" -type "double3" -3.9756933518293935e-16 9.8398410457777553e-15 3.1308585145656493e-15 ;
	setAttr ".rst" -type "double3" 5.5213954127458571 1.7559403927372124 -0.086046692341986919 ;
	setAttr ".rsrr" -type "double3" -2.3854160110976364e-15 1.3219180394832742e-14 9.3925755436969478e-15 ;
	setAttr -k on ".w0";
createNode joint -n "loFingerB1_L" -p "loWrist_L";
	rename -uid "F48B8FDC-43AE-99CB-B423-AC9846ADF986";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -2.8335492771707829 15.298551737982526 9.9358856464689005 ;
	setAttr ".bps" -type "matrix" 0.9523620455980677 -0.1778396608006684 -0.24774904470162376 0
		 0.1721112870101561 0.98405947739265376 -0.04477331613142508 0 0.25176226680734282 -2.1766368342354017e-16 0.96778910978168609 0
		 39.660000000000018 14.528000000000004 -1.4829999999999168 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "loFingerB2_L" -p "loFingerB1_L";
	rename -uid "2C73F1E7-42C1-63B7-FFE3-A187F676FE70";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 1.3643449526124998 -5.6302836786381905 -3.4011530161809387 ;
	setAttr ".bps" -type "matrix" 0.96063681300616799 -0.23476909644539837 -0.14852738754709144 0
		 0.23201230989430563 0.97205116704534167 -0.035872227464955719 0 0.15279791083277747 -4.6549294395507589e-17 0.98825745554745936 0
		 41.213000000000015 14.238000000000003 -1.8869999999999194 1;
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode joint -n "loFingerBEnd_L" -p "loFingerB2_L";
	rename -uid "B505B973-4A2F-3E77-D2F4-179B63E59BE5";
	setAttr ".t" -type "double3" 1.2522942944851219 1.1041514924592377e-15 3.4694469519536142e-15 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 1;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "loFingerB2_L_parentConstraint" -p "loFingerB2_L";
	rename -uid "838F1437-4131-208C-3FEB-77B12B6C82F3";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loFingerB2_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 7.1054273576010019e-15 -3.5527136788005009e-15 
		2.6645352591003757e-15 ;
	setAttr ".tg[0].tor" -type "double3" -5.9635400277440939e-16 4.7708320221952744e-15 
		2.7581372628316426e-15 ;
	setAttr ".lr" -type "double3" 1.8636062586700296e-15 -6.3673213837892666e-15 -6.3983814881004342e-15 ;
	setAttr ".rst" -type "double3" 1.6306823725054471 -7.1054273576010019e-15 0 ;
	setAttr ".rsrr" -type "double3" 5.7150591932547584e-16 -4.783256063919742e-15 -3.5656999749219899e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loFingerB1_L_parentConstraint" -p "loFingerB1_L";
	rename -uid "21A5C08E-4C6A-5772-DE46-D4A9CA66EAC3";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loFingerB1_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -2.1316282072803006e-14 0 -3.5527136788005009e-15 ;
	setAttr ".tg[0].tor" -type "double3" -4.3732626870123352e-15 -3.1805546814635168e-15 
		1.1131941385122309e-14 ;
	setAttr ".lr" -type "double3" 4.5720473546038059e-15 6.6592863643142393e-15 -1.3914926731402889e-14 ;
	setAttr ".rst" -type "double3" 6.7082514341586084 -1.7559403927371982 0.086046692342076625 ;
	setAttr ".rsrr" -type "double3" 5.1684013573782159e-15 6.5598940305185051e-15 -1.2225257056875397e-14 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loWrist_L_parentConstraint" -p "loWrist_L";
	rename -uid "9D185933-44B7-F4E7-75F0-D3BC62EC2C9C";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loWrist_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -7.1054273576010019e-15 0 0 ;
	setAttr ".tg[0].tor" -type "double3" 0 8.9453100416161419e-16 1.3284406613886192e-14 ;
	setAttr ".lr" -type "double3" -7.4544250346801113e-17 -6.2741410708557658e-16 -1.2720665720638509e-14 ;
	setAttr ".rst" -type "double3" 8.4152140198571423 7.1054273576010019e-15 2.2204460492503131e-15 ;
	setAttr ".rsrr" -type "double3" -2.4848083448933672e-17 -6.2120208622334314e-16 
		-9.5403051648269372e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loElbow_L_parentConstraint" -p "loElbow_L";
	rename -uid "C5B2D760-441F-53F3-1E39-2CA16B504F0F";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loElbow_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -1.7763568394002505e-14 -1.0658141036401503e-14 
		0 ;
	setAttr ".tg[0].tor" -type "double3" 1.3666445896913547e-16 1.5902773407317584e-15 
		1.1430118386509513e-15 ;
	setAttr ".lr" -type "double3" -7.4544250346801162e-17 -7.9669167558143755e-16 -1.1911550003332605e-15 ;
	setAttr ".rst" -type "double3" 9.1427725554122699 -7.1054273576010019e-15 3.8857805861880479e-16 ;
	setAttr ".rsrr" -type "double3" -7.4544250346801162e-17 -7.9669167558143755e-16 
		-1.1911550003332605e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loShoulder_L_parentConstraint" -p "loShoulder_L";
	rename -uid "F5F5D549-4A2E-2429-D15F-23905F928442";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loShoulder_L_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 3.5527136788005009e-15 3.0531133177191805e-16 ;
	setAttr ".tg[0].tor" -type "double3" -1.1678599220998848e-14 -2.3854160110976376e-15 
		-1.1927080055488187e-14 ;
	setAttr ".lr" -type "double3" 6.3611093629270335e-15 9.5416640443905487e-15 9.5416640443905487e-15 ;
	setAttr ".rst" -type "double3" 0.50000000000000355 -1.9999999999999996 16.5 ;
	setAttr ".rsrr" -type "double3" 6.3611093629270335e-15 9.5416640443905487e-15 9.5416640443905487e-15 ;
	setAttr -k on ".w0";
createNode joint -n "loShoulder_R" -p "spine";
	rename -uid "5D315FE9-431A-0EF6-53E0-23A31B299D0F";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 49.207034055317557 -83.555512044930111 40.613406637111069 ;
	setAttr ".bps" -type "matrix" 0.99368106829059411 0.08520391328545629 0.073063175962368568 0
		 0.084974522483502582 -0.99636353464026595 0.0062479891050055691 0 0.073329837375820661 5.5511151231257837e-16 -0.99730774335228922 0
		 -16.5 19.5 -1 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "loElbow_R" -p "loShoulder_R";
	rename -uid "BF8EFFDA-4E2E-68D8-C664-AD81032DCF9A";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.097090872588108584 -4.2245823943083796 3.5699958052975154 ;
	setAttr ".bps" -type "matrix" 0.99973690272738092 0.02293465139978406 -0.00035649717201723108 0
		 0.022934649941631951 -0.9997369662892186 -8.1782894275734489e-06 0 -0.00035659096746022689 -1.0337715043374371e-13 -0.99999993642143881 0
		 -25.585000000000004 18.721000000000004 -1.6680000000000004 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "loWrist_R" -p "loElbow_R";
	rename -uid "07E716BB-4D69-7AE5-F56F-82B4A76C4595";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.33378093321428698 -0.88979687962197629 -19.248887884355785 ;
	setAttr ".bps" -type "matrix" 0.93616767052418659 0.35119575027525257 -0.015863090932106839 0
		 0.35114534289864341 -0.93630205862670313 -0.0059500564696206319 0 -0.016942279241942731 -1.3481574405928464e-13 -0.99985646928651106 0
		 -33.998000000000005 18.527999999999999 -1.6650000000000003 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "loThumb1_R" -p "loWrist_R";
	rename -uid "90AAA61E-45AB-FD87-0CD6-0584957F882C";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 90.868851202964152 -12.887106561830638 81.11115451882894 ;
	setAttr ".bps" -type "matrix" 0.47542194344605915 -0.84885696101420949 -0.23111866092484426 0
		 -0.11293657638623174 0.20164613842533669 -0.97292556990363188 0 0.87247882805494414 0.48865191557577181 8.6215756756047313e-16 0
		 -35.662999999999997 21.808 -0.66099999999999981 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "loThumb2_R" -p "loThumb1_R";
	rename -uid "73BDDF5A-4A8D-8D5A-A604-D5A5DE92B1D8";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.92887510270754481 16.780262712054846 -10.148617095669962 ;
	setAttr ".bps" -type "matrix" 0.2152222238352976 -0.97508844268237949 -0.053683547895433531 0
		 -0.011570577349479838 0.05242179942009835 -0.99855799865874528 0 0.97649655202012287 0.21553302274317798 1.2416712038667635e-15 0
		 -36.888999999999996 23.996999999999986 -0.065000000000001501 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "loThumbEnd_R" -p "loThumb2_R";
	rename -uid "F27034E1-4A96-0CEE-0982-6F928C1F8DE1";
	setAttr ".t" -type "double3" -2.0490448994592683 1.4210854715202004e-14 0 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "loThumb2_R_parentConstraint" -p "loThumb2_R";
	rename -uid "E28E3941-4857-CD5C-CB17-ABA4C2240815";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loThumb2_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -1.0658141036401503e-14 8.8817841970012523e-16 
		0 ;
	setAttr ".tg[0].tor" -type "double3" -3.0811623476677818e-15 -1.2722218725854067e-14 
		1.0883460550632971e-14 ;
	setAttr ".lr" -type "double3" 3.975693351829392e-15 3.8390288928602603e-14 -1.2722218725854065e-14 ;
	setAttr ".rst" -type "double3" -2.5787619122361676 1.7763568394002505e-14 -1.0658141036401503e-14 ;
	setAttr ".rsrr" -type "double3" 3.5781240166464552e-15 1.2771914892751935e-14 -1.0933156717530839e-14 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loThumb1_R_parentConstraint" -p "loThumb1_R";
	rename -uid "5FC8FF4C-4BE3-6550-DFC3-C8BEC67372FA";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loThumb1_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -7.1054273576010019e-15 1.7763568394002505e-15 
		-3.5527136788005009e-15 ;
	setAttr ".tg[0].tor" -type "double3" 1.6896696745274934e-15 -1.1604054970652047e-14 
		1.2722218725854064e-14 ;
	setAttr ".lr" -type "double3" 1.2722218725854064e-14 4.4527765540489241e-14 -9.5416640443905456e-15 ;
	setAttr ".rst" -type "double3" -0.42272365381577259 -3.6617016049173188 -0.9756470002262656 ;
	setAttr ".rsrr" -type "double3" -3.1805546814635168e-15 1.9083328088781097e-14 -6.361109362927032e-15 ;
	setAttr -k on ".w0";
createNode joint -n "loFingerA1_R" -p "loWrist_R";
	rename -uid "BCE69563-45C3-269F-60EA-5E801B68309D";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.10759895188056175 15.754415943653242 20.956755684058326 ;
	setAttr ".bps" -type "matrix" 0.96687323370463063 -0.0066566143456458279 0.2551702165829775 0
		 -0.0064362448361361202 -0.99997784449729343 -0.001698607357799257 0 0.25517587013265902 1.2024411036691527e-14 -0.96689465574179279 0
		 -39.784999999999997 18.233000000000004 -1.6530000000000005 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "loFingerA2_R" -p "loFingerA1_R";
	rename -uid "BE9D904B-4BE2-395B-A73D-329846EE30E5";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 0.51982383854029102 6.8774812014232856 3.9514721757772704 ;
	setAttr ".bps" -type "matrix" 0.92663734741845627 -0.075007177835449468 0.36839808582896816 0
		 -0.06970079988700062 -0.99718299387482601 -0.027710561559648017 0 0.36943879718351041 -1.0801791146666935e-14 -0.92925506462736096 0
		 -41.528000000000006 18.245000000000001 -2.1130000000000004 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "loFingerAEnd_R" -p "loFingerA2_R";
	rename -uid "4E8E7AFD-4D59-CA50-D718-AD834C2D2D0B";
	setAttr ".t" -type "double3" -1.5065224857266344 1.7763568394002505e-15 8.8817841970012523e-15 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "loFingerA2_R_parentConstraint" -p "loFingerA2_R";
	rename -uid "BB9CC176-41CD-3C8C-86BA-6F910871C94C";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loFingerA2_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 7.1054273576010019e-15 0 3.5527136788005009e-15 ;
	setAttr ".tg[0].tor" -type "double3" -2.981770013872047e-16 -2.385416011097638e-15 
		-1.5405811738338911e-15 ;
	setAttr ".lr" -type "double3" 3.478731682850721e-16 3.1727896553857243e-15 1.5995953720251081e-15 ;
	setAttr ".rst" -type "double3" -1.8027182253474976 7.1054273576010019e-15 -1.7763568394002505e-15 ;
	setAttr ".rsrr" -type "double3" 3.478731682850721e-16 3.1727896553857243e-15 1.5995953720251081e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loFingerA1_R_parentConstraint" -p "loFingerA1_R";
	rename -uid "492D564F-4AF7-7916-34C5-21B7FE3377CA";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loFingerA1_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -1.4210854715202004e-14 -3.5527136788005009e-15 
		-3.5527136788005009e-15 ;
	setAttr ".tg[0].tor" -type "double3" -6.2120208622334312e-17 1.2722218725854067e-14 
		3.7706966633756929e-15 ;
	setAttr ".lr" -type "double3" -3.9756933518293984e-16 -9.9392333795734903e-15 3.2302508483613843e-15 ;
	setAttr ".rst" -type "double3" -5.5213954127458535 -1.7559403927372124 0.086046692341724906 ;
	setAttr ".rsrr" -type "double3" -3.9756933518293984e-16 -9.9392333795734903e-15 
		3.2302508483613843e-15 ;
	setAttr -k on ".w0";
createNode joint -n "loFingerB1_R" -p "loWrist_R";
	rename -uid "2280CAAB-4AC4-B3C0-AF60-1AB50D742AEC";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -2.8335492771617696 15.298551737981017 9.9358856464712488 ;
	setAttr ".bps" -type "matrix" 0.9523620455980677 0.1778396608006681 0.24774904470162376 0
		 0.17211128701015604 -0.98405947739265387 0.044773316131423345 0 0.25176226680734265 1.9734266115500076e-15 -0.96778910978168597 0
		 -39.659999999999989 14.527999999999999 -1.4830000000000001 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "loFingerB2_R" -p "loFingerB1_R";
	rename -uid "7A515348-4437-E558-DCD3-F5A5D69601B3";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 1.364344952611894 -5.6302836786382358 -3.4011530161809671 ;
	setAttr ".bps" -type "matrix" 0.96063681300616799 0.23476909644539865 0.14852738754709074 0
		 0.23201230989430438 -0.97205116704534178 0.035872227464964559 0 0.15279791083277899 -8.6878527322407504e-15 -0.98825745554745903 0
		 -41.213000000000001 14.238000000000003 -1.8869999999999987 1;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode joint -n "loFingerBEnd_R" -p "loFingerB2_R";
	rename -uid "A4FD87B7-4D59-5E70-113C-81816944C864";
	setAttr ".t" -type "double3" -1.2522942944851181 0 -1.7763568394002505e-15 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".jo" -type "double3" 8.5377364625159429e-07 3.2064975146932664e-22 -5.9242447254015629e-22 ;
	setAttr ".sd" 2;
	setAttr ".radi" 0.4;
createNode parentConstraint -n "loFingerB2_R_parentConstraint" -p "loFingerB2_R";
	rename -uid "21C12B99-4FA2-119A-E33B-2890E40BABC3";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loFingerB2_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 3.5527136788005009e-15 -4.4408920985006262e-15 ;
	setAttr ".tg[0].tor" -type "double3" 0 3.1805546814635176e-15 -9.4422717105948169e-16 ;
	setAttr ".lr" -type "double3" -3.975693351829396e-16 -5.5721827134233874e-15 4.8453762725420762e-16 ;
	setAttr ".rst" -type "double3" -1.6306823725054613 -3.5527136788005009e-15 -7.1054273576010019e-15 ;
	setAttr ".rsrr" -type "double3" -2.484808344893373e-16 -3.1805546814635168e-15 8.4483483726374669e-16 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loFingerB1_R_parentConstraint" -p "loFingerB1_R";
	rename -uid "458598FC-4BED-3116-1B63-E8A9DD415431";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loFingerB1_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 -7.1054273576010019e-15 -1.7763568394002505e-15 ;
	setAttr ".tg[0].tor" -type "double3" 4.3732626870123352e-15 -3.1805546814635168e-15 
		-1.6896696745274934e-15 ;
	setAttr ".lr" -type "double3" -5.7647553601526243e-15 3.5284278497485889e-15 2.5842006786891072e-15 ;
	setAttr ".rst" -type "double3" -6.7082514341585728 1.7559403927372266 -0.086046692341727127 ;
	setAttr ".rsrr" -type "double3" -5.7647553601526243e-15 3.5284278497485889e-15 2.5842006786891076e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loWrist_R_parentConstraint" -p "loWrist_R";
	rename -uid "19664CAB-410D-39B1-DD19-289016AA7FB3";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loWrist_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 1.4210854715202004e-14 7.1054273576010019e-15 
		0 ;
	setAttr ".tg[0].tor" -type "double3" 0 9.9392333795734899e-17 -1.501756043444932e-15 ;
	setAttr ".lr" -type "double3" 4.9696166897867449e-17 -1.9878466759146975e-16 -6.3613034885789771e-15 ;
	setAttr ".rst" -type "double3" -8.4152140198571352 7.1054273576010019e-15 -8.8817841970012523e-16 ;
	setAttr ".rsrr" -type "double3" 4.9696166897867449e-17 -1.9878466759146975e-16 -6.3613034885789771e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loElbow_R_parentConstraint" -p "loElbow_R";
	rename -uid "1D74ABC1-4C5B-0E3F-5076-298C7A69081B";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loElbow_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 3.5527136788005009e-15 -3.5527136788005009e-15 
		-2.2204460492503131e-16 ;
	setAttr ".tg[0].tor" -type "double3" 1.6151254241806922e-16 7.9513867036587919e-16 
		1.5576642312050331e-15 ;
	setAttr ".lr" -type "double3" -7.4544250346801162e-17 -7.9824468079699591e-16 -1.5887243355162001e-15 ;
	setAttr ".rst" -type "double3" -9.1427725554122894 -7.1054273576010019e-15 -1.609823385706477e-15 ;
	setAttr ".rsrr" -type "double3" -7.4544250346801162e-17 -7.9824468079699591e-16 
		-1.5887243355162001e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "loShoulder_R_parentConstraint" -p "loShoulder_R";
	rename -uid "AD062DEB-417A-B0B0-2BA9-0AAAAE5CF767";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "loShoulder_R_CTLW0" -dv 1 -min 0 
		-at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 0 -1.3877787807814457e-16 ;
	setAttr ".tg[0].tor" -type "double3" -1.130090835257506e-13 5.5659706925611567e-15 
		-4.0552072188659847e-14 ;
	setAttr ".lr" -type "double3" 9.2236085762441989e-14 -1.7920459570083645e-29 2.2263882770244617e-14 ;
	setAttr ".rst" -type "double3" 0.5 -2 -16.5 ;
	setAttr ".rsrr" -type "double3" 9.2236085762441989e-14 -1.7920459570083648e-29 2.2263882770244617e-14 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "spine_parentConstraint" -p "spine";
	rename -uid "1C5E51D8-42A2-8D73-7C2F-E1A6DAD604C5";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "spine_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".rst" -type "double3" 8 0 0 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "hips_parentConstraint" -p "hips";
	rename -uid "23778568-4A86-8163-22F3-4CAE8C742F5D";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "hips_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr -k on ".w0";
createNode joint -n "leg_L" -p "root";
	rename -uid "5F543420-4232-3AC0-ACEF-1996A0A9CFFF";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -179.29284226956358 -5.666783353597002 172.87498365109818 ;
	setAttr ".bps" -type "matrix" 0.098742859722262044 -0.9874285972226251 0.1234285746528285 0
		 -0.012281602211417567 0.12281602211417433 0.99235345868252567 0 -0.99503719020998926 -0.099503719020998499 -1.8821749714348357e-16 0
		 7.7999999999999998 9 -0.5 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "knee_L" -p "leg_L";
	rename -uid "694F91EA-42B6-C47D-7A4D-ABB598000D55";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" 8.4713084826764083e-12 -1.0751297105969699 -7.090016997268914 ;
	setAttr ".bps" -type "matrix" 0.080815868766054913 -0.99672904811467611 7.8750229504166158e-16 0
		 -2.6194324487249787e-16 7.2164496600635175e-16 1 0 -0.996729048114676 -0.080815868766054913 -2.0249583030587327e-16 0
		 8.1999999999999993 5 0 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "ankle_L" -p "knee_L";
	rename -uid "98D70CED-4E57-8CEC-890F-9E8600DDC15E";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -4.6354634269025121 2.6637145457256954e-14 90.000000000000185 ;
	setAttr ".bps" -type "matrix" -5.0214090146212198e-16 3.5982221934442668e-15 1 0
		 -2.3592239273284576e-15 1.0000000000000002 -3.6551414005027326e-15 0 -1 -2.3592239273284576e-15 -4.944596571459547e-16 0
		 8.5000000000000018 1.2999999999999994 5.1834278059707023e-15 1;
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode joint -n "footEnd_L" -p "ankle_L";
	rename -uid "533268C6-4977-16AE-ED78-69AADD915FD5";
	setAttr ".t" -type "double3" 4.9999999999999991 -3.2844730064418794e-16 -7.9442804708629913e-17 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 1;
	setAttr ".radi" 1.2;
createNode parentConstraint -n "ankle_L_parentConstraint" -p "ankle_L";
	rename -uid "8E8155C5-4EED-B03C-786C-9DB4585F8BA8";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "ankle_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 0 -2.2204460492503131e-16 3.5527136788005009e-15 ;
	setAttr ".tg[0].tor" -type "double3" 2.3854160110976376e-15 -3.9626890501647598e-16 
		-3.2129911217551219e-17 ;
	setAttr ".lr" -type "double3" -1.5902773407317584e-15 -3.975693351829396e-16 5.5173828725626983e-33 ;
	setAttr ".rst" -type "double3" 3.7121422386541161 2.2601072735094939e-15 -1.7763568394002505e-15 ;
	setAttr ".rsrr" -type "double3" -1.1927080055488188e-15 3.975693351829396e-16 1.2424041724466858e-17 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "knee_L_parentConstraint" -p "knee_L";
	rename -uid "4F6E508F-4C5D-CDB4-9009-4A86151A9459";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "knee_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 1.7763568394002505e-15 1.9721522630525295e-31 
		1.7763568394002505e-15 ;
	setAttr ".tg[0].tor" -type "double3" -2.4848083448933719e-17 1.3914926731402886e-15 
		-4.6258928086869903e-19 ;
	setAttr ".lr" -type "double3" -2.4848083448933716e-17 -1.1973670211954938e-15 -7.9494454471393441e-16 ;
	setAttr ".rst" -type "double3" 4.0509258201058183 -1.6653345369377348e-15 1.7763568394002505e-15 ;
	setAttr ".rsrr" -type "double3" -2.4848083448933716e-17 -1.1973670211954938e-15 
		-7.9494454471393441e-16 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "leg_L_parentConstraint" -p "leg_L";
	rename -uid "36D51BD7-4870-5512-4F73-8AB705974F32";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "leg_L_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" -1.7763568394002505e-15 1.1102230246251565e-16 
		0 ;
	setAttr ".tg[0].tor" -type "double3" -9.9392333795734874e-17 3.1805546814635168e-15 
		-2.3605679276487037e-16 ;
	setAttr ".lr" -type "double3" 0 -3.1929787231879836e-15 0 ;
	setAttr ".rst" -type "double3" -1.9999999999999982 -1.5 7.8 ;
	setAttr ".rsrr" -type "double3" 0 -3.1929787231879836e-15 0 ;
	setAttr -k on ".w0";
createNode joint -n "leg_R" -p "root";
	rename -uid "1C13CD41-4448-EBCD-33F3-51BB836B0131";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -179.29284226956307 -5.6667833535970207 -7.125016348901843 ;
	setAttr ".bps" -type "matrix" 0.098742859722262377 0.98742859722262488 -0.12342857465282892 0
		 -0.012281602211426407 -0.12281602211417389 -0.99235345868252578 0 -0.99503719020998926 0.099503719020999915 8.9225501986867089e-15 0
		 -7.7999999999999998 9 -0.5 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "knee_R" -p "leg_R";
	rename -uid "EA73F53A-4835-8B74-A046-08879EB0219A";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -7.938409636979035e-12 -1.0751297105970576 -7.0900169972689531 ;
	setAttr ".bps" -type "matrix" 0.080815868766054802 0.99672904811467589 -3.3218251726239021e-16 0
		 -8.9251522839006725e-15 3.7470027081099033e-16 -1.0000000000000002 0 -0.99672904811467611 0.080815868766054816 8.9300866903142614e-15 0
		 -8.1999999999999993 4.9999999999999991 1.1102230246251565e-16 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "ankle_R" -p "knee_R";
	rename -uid "6604A0A1-452A-3494-5FBA-DFA55A187170";
	addAttr -ci true -sn "liw" -ln "lockInfluenceWeights" -min 0 -max 1 -at "bool";
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jo" -type "double3" -4.6354634269021187 5.0053979299532107e-13 90.000000000000185 ;
	setAttr ".bps" -type "matrix" -9.1874070015623387e-15 -2.9456351175170157e-15 -1.0000000000000002 0
		 -9.1593399531575415e-15 -1.0000000000000002 2.9400723240729015e-15 0 -1.0000000000000002 9.1732177409653559e-15 9.1983392873923087e-15 0
		 -8.5 1.2999999999999994 5.2862199999999867e-15 1;
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode joint -n "footEnd_R" -p "ankle_R";
	rename -uid "D6748545-401A-8B94-45A8-C0822BCE6840";
	setAttr ".t" -type "double3" -4.9999999999999938 1.3766765505351941e-14 4.6185277824406512e-14 ;
	setAttr ".mnrl" -type "double3" -360 -360 -360 ;
	setAttr ".mxrl" -type "double3" 360 360 360 ;
	setAttr ".jot" -type "string" "none";
	setAttr ".sd" 2;
	setAttr ".radi" 1.2;
createNode parentConstraint -n "ankle_R_parentConstraint" -p "ankle_R";
	rename -uid "94CB2A14-4E57-B262-9EF9-10A604A1A9BE";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "ankle_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 2.5243548967072378e-29 -2.2204460492503131e-16 
		-1.7763568394002505e-15 ;
	setAttr ".tg[0].tor" -type "double3" -2.385416011097638e-15 3.9626890501647632e-16 
		3.2129911217525846e-17 ;
	setAttr ".lr" -type "double3" 3.5781240166464568e-15 0 0 ;
	setAttr ".rst" -type "double3" -3.712142238654117 -3.9420889442652884e-15 0 ;
	setAttr ".rsrr" -type "double3" 3.1805546814635168e-15 0 0 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "knee_R_parentConstraint" -p "knee_R";
	rename -uid "7C102BAE-4B23-DB8C-D1F7-C0B61BDAC07E";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "knee_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 1.7763568394002505e-15 1.1102230246251565e-16 
		0 ;
	setAttr ".tg[0].tor" -type "double3" 7.4544250346801162e-17 3.975693351829396e-16 
		3.1805546814635168e-15 ;
	setAttr ".lr" -type "double3" -7.4544250346801125e-17 -1.1973670211954938e-15 -3.9753051005255064e-15 ;
	setAttr ".rst" -type "double3" -4.0509258201058191 3.3306690738754696e-15 -1.7763568394002505e-15 ;
	setAttr ".rsrr" -type "double3" -4.9696166897867431e-17 -5.9790700798996775e-16 
		-3.1802634929855994e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "leg_R_parentConstraint" -p "leg_R";
	rename -uid "4F43403A-46C2-DA9A-2075-F7A15F31EC49";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "leg_R_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".tg[0].tot" -type "double3" 1.7763568394002505e-15 -2.2204460492503131e-16 
		0 ;
	setAttr ".tg[0].tor" -type "double3" 1.987846675914698e-16 -3.975693351829396e-15 
		-5.1311292322048145e-15 ;
	setAttr ".lr" -type "double3" -3.478731682850721e-16 1.5902773407317584e-15 3.1805546814635168e-15 ;
	setAttr ".rst" -type "double3" -1.9999999999999982 -1.5 -7.8 ;
	setAttr ".rsrr" -type "double3" -3.4787316828507205e-16 2.3854160110976376e-15 3.1805546814635168e-15 ;
	setAttr -k on ".w0";
createNode parentConstraint -n "root_parentConstraint" -p "root";
	rename -uid "4BD55232-4B6B-EB2F-0C48-948ACBF206B2";
	addAttr -dcb 0 -ci true -k true -sn "w0" -ln "cog_CTLW0" -dv 1 -min 0 -at "double";
	setAttr -k on ".nds";
	setAttr -k off ".v";
	setAttr -k off ".tx";
	setAttr -k off ".ty";
	setAttr -k off ".tz";
	setAttr -k off ".rx";
	setAttr -k off ".ry";
	setAttr -k off ".rz";
	setAttr -k off ".sx";
	setAttr -k off ".sy";
	setAttr -k off ".sz";
	setAttr ".erp" yes;
	setAttr ".rst" -type "double3" 0 11 1 ;
	setAttr -k on ".w0";
createNode transform -n "controls_GRP" -p "main_CTL";
	rename -uid "2B04B30C-4856-34B1-AF47-76BA561FDB12";
createNode transform -n "cog_CTL_GRP" -p "controls_GRP";
	rename -uid "A3678D9A-4CDC-3DDD-A238-44BF2779D660";
	setAttr ".t" -type "double3" 0 11 1 ;
	setAttr ".r" -type "double3" 90 0 90 ;
createNode transform -n "cog_CTL" -p "cog_CTL_GRP";
	rename -uid "89DA9BCE-4AF7-7157-1F74-BA8C8CCF9745";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "cog_CTLShape" -p "cog_CTL";
	rename -uid "ADDF35E2-446B-D837-644E-7082AA0764D7";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 17;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.0166309058134887e-15 16.602842656695906 -16.602842656695902
		8.8035768984536566e-32 23.479965259045887 -1.4377332149290798e-15
		-1.0166309058134887e-15 16.602842656695898 16.602842656695902
		-1.4377332149290804e-15 1.2172055452800943e-15 23.479965259045894
		-1.0166309058134887e-15 -16.602842656695902 16.602842656695902
		-1.4401871945021588e-31 -23.479965259045901 2.3520041786821688e-15
		1.0166309058134887e-15 -16.602842656695898 -16.602842656695902
		1.4377332149290804e-15 -3.2019545721209692e-15 -23.479965259045894
		1.0166309058134887e-15 16.602842656695906 -16.602842656695902
		8.8035768984536566e-32 23.479965259045887 -1.4377332149290798e-15
		-1.0166309058134887e-15 16.602842656695898 16.602842656695902
		;
	setAttr ".adot" yes;
createNode transform -n "hips_CTL_GRP" -p "cog_CTL";
	rename -uid "6E903D8C-4232-2759-5E57-B8B44B6C63FE";
createNode transform -n "hips_CTL" -p "hips_CTL_GRP";
	rename -uid "B098DC97-4E78-E03E-207F-C5AD9CB3F427";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "hips_CTLShape" -p "hips_CTL";
	rename -uid "E807774C-4515-0809-BADC-538D10A66161";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 17;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		8.486925988788922e-16 13.860201969576618 -13.860201969576616
		7.3493049588141668e-32 19.601285602605529 -1.200232583620198e-15
		-8.486925988788922e-16 13.860201969576615 13.860201969576616
		-1.2002325836201984e-15 1.0161341069667252e-15 19.601285602605536
		-8.486925988788922e-16 -13.860201969576616 13.860201969576616
		-1.2022811877788582e-31 -19.60128560260554 1.9634741847460562e-15
		8.486925988788922e-16 -13.860201969576615 -13.860201969576616
		1.2002325836201984e-15 -2.6730203968479833e-15 -19.601285602605536
		8.486925988788922e-16 13.860201969576618 -13.860201969576616
		7.3493049588141668e-32 19.601285602605529 -1.200232583620198e-15
		-8.486925988788922e-16 13.860201969576615 13.860201969576616
		;
	setAttr ".adot" yes;
createNode transform -n "spine_CTL_GRP" -p "hips_CTL";
	rename -uid "53A4139D-4DEC-16AE-8316-9B8017F6A7DF";
	setAttr ".t" -type "double3" 8 0 0 ;
createNode transform -n "spine_CTL" -p "spine_CTL_GRP";
	rename -uid "7DADA51C-4742-7DCC-4E25-9FA7429C4B12";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "spine_CTLShape" -p "spine_CTL";
	rename -uid "77D1816F-480F-4AAE-AA19-2482057B59BD";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 17;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.0052956747173113e-15 16.417724284540448 -16.417724284540444
		8.7054187782880177e-32 23.218168346499208 -1.4217027773802323e-15
		-1.0052956747173113e-15 16.417724284540441 16.417724284540444
		-1.4217027773802329e-15 1.2036339471038042e-15 23.218168346499215
		-1.0052956747173113e-15 -16.417724284540444 16.417724284540444
		-1.4241293955722959e-31 -23.218168346499223 2.3257798029012613e-15
		1.0052956747173113e-15 -16.417724284540441 -16.417724284540444
		1.4217027773802329e-15 -3.1662534195916634e-15 -23.218168346499215
		1.0052956747173113e-15 16.417724284540448 -16.417724284540444
		8.7054187782880177e-32 23.218168346499208 -1.4217027773802323e-15
		-1.0052956747173113e-15 16.417724284540441 16.417724284540444
		;
	setAttr ".adot" yes;
createNode transform -n "head_CTL_GRP" -p "spine_CTL";
	rename -uid "60B1D123-4F25-5886-549A-1BAA171FF8F1";
	setAttr ".t" -type "double3" 9 0 0 ;
createNode transform -n "head_CTL" -p "head_CTL_GRP";
	rename -uid "AC933755-49E3-EE8C-8C50-DC992866D31A";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "head_CTLShape" -p "head_CTL";
	rename -uid "7590E4A3-4EBA-C16C-7D70-ECAE466F5A16";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 17;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.0737874236865126e-15 17.536279430675446 -17.536279430675442
		9.2985272265084286e-32 24.800044204425543 -1.5185647376831304e-15
		-1.0737874236865126e-15 17.536279430675442 17.536279430675442
		-1.518564737683131e-15 1.285638670917049e-15 24.800044204425554
		-1.0737874236865126e-15 -17.536279430675442 17.536279430675442
		-1.5211566836769885e-31 -24.800044204425557 2.4842373894841796e-15
		1.0737874236865126e-15 -17.536279430675442 -17.536279430675442
		1.518564737683131e-15 -3.3819732718117869e-15 -24.800044204425554
		1.0737874236865126e-15 17.536279430675446 -17.536279430675442
		9.2985272265084286e-32 24.800044204425543 -1.5185647376831304e-15
		-1.0737874236865126e-15 17.536279430675442 17.536279430675442
		;
	setAttr ".adot" yes;
createNode transform -n "upShoulder_L_CTL_GRP" -p "head_CTL";
	rename -uid "0C5AE75C-4155-DB55-D078-43A4046FCF0D";
	setAttr ".t" -type "double3" 7.5 -5.8 16 ;
	setAttr ".r" -type "double3" 49.289957417995161 -84.982002891423036 -139.39870535499568 ;
createNode transform -n "upShoulder_L_CTL" -p "upShoulder_L_CTL_GRP";
	rename -uid "750923BF-438C-8215-A0C6-4BB99FE0869E";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upShoulder_L_CTLShape" -p "upShoulder_L_CTL";
	rename -uid "72F05FE7-4B94-56EA-CA75-90814FC8D366";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		3.4999999999999969 5.0053666945844411 -5.0053666945844366
		3.499999999999996 7.0786574641319024 -2.0887665867695716e-15
		3.4999999999999964 5.0053666945844428 5.0053666945844331
		3.4999999999999978 9.5717992468729885e-15 7.0786574641318989
		3.4999999999999996 -5.005366694584426 5.0053666945844366
		3.5000000000000004 -7.0786574641318918 3.2525762056818239e-15
		3.5 -5.0053666945844286 -5.0053666945844331
		3.4999999999999987 4.0407003022001799e-15 -7.0786574641318971
		3.4999999999999969 5.0053666945844411 -5.0053666945844366
		3.499999999999996 7.0786574641319024 -2.0887665867695716e-15
		3.4999999999999964 5.0053666945844428 5.0053666945844331
		;
	setAttr ".adot" yes;
createNode transform -n "upElbow_L_CTL_GRP" -p "upShoulder_L_CTL";
	rename -uid "DF450B41-4DEB-40CC-EB26-AAB26F47C785";
	setAttr ".t" -type "double3" 10.540398474441092 -7.1054273576010019e-15 2.6645352591003757e-15 ;
	setAttr ".r" -type "double3" -0.07877427673140007 -3.8543147501225117 4.9796103462861554 ;
	setAttr ".s" -type "double3" 1.0000000000000002 1.0000000000000002 1 ;
createNode transform -n "upElbow_L_CTL" -p "upElbow_L_CTL_GRP";
	rename -uid "DB42BC93-4D85-0579-29A9-19A9355EA879";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upElbow_L_CTLShape" -p "upElbow_L_CTL";
	rename -uid "C5A84C86-4D12-3DBF-BF07-21A80D8C4CC3";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		7.2624512150244642e-15 4.7087140365722266 -4.708714036572224
		5.4340743120997051e-15 6.6591272518570008 5.8542826989400002e-16
		2.5036218659031088e-15 4.708714036572224 4.708714036572224
		1.8771317552722638e-16 -1.5361503006491849e-15 6.6591272518570017
		-1.570238574234608e-16 -4.7087140365722258 4.7087140365722222
		1.6713530455012955e-15 -6.6591272518570044 -2.1024897217951493e-15
		4.6018054916978927e-15 -4.708714036572224 -4.7087140365722258
		6.9177141820737751e-15 9.7325780115364711e-16 -6.6591272518570035
		7.2624512150244642e-15 4.7087140365722266 -4.708714036572224
		5.4340743120997051e-15 6.6591272518570008 5.8542826989400002e-16
		2.5036218659031088e-15 4.708714036572224 4.708714036572224
		;
	setAttr ".adot" yes;
createNode transform -n "upWrist_L_CTL_GRP" -p "upElbow_L_CTL";
	rename -uid "E626F1F4-41EC-74F1-60EA-BDBBB0D5944C";
	setAttr ".t" -type "double3" 9.8025506884687807 -7.1054273576010019e-15 8.8817841970012523e-16 ;
	setAttr ".r" -type "double3" 0.038334688053347228 2.8749852142838814 -0.40482835539985584 ;
createNode transform -n "upWrist_L_CTL" -p "upWrist_L_CTL_GRP";
	rename -uid "BB6B34DA-4AF5-138B-B2CD-3C8B5BB725F5";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upWrist_L_CTLShape" -p "upWrist_L_CTL";
	rename -uid "C4C4DA80-455F-B506-A8B9-41A6F58939A3";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.2142996497326259e-15 4.5247932275672422 -4.5247932275672369
		4.1671893130569511e-15 6.3990239493595178 4.219451313044492e-15
		4.6789959937687369e-15 4.524793227567236 4.5247932275672422
		2.4499102796201745e-15 -3.8354627815392038e-15 6.3990239493595205
		-1.2142996497326259e-15 -4.5247932275672422 4.5247932275672369
		-4.1671893130569535e-15 -6.3990239493595213 -3.082105497706674e-15
		-4.6789959937687369e-15 -4.524793227567236 -4.5247932275672422
		-2.4499102796201745e-15 3.2945566764362682e-15 -6.3990239493595205
		1.2142996497326259e-15 4.5247932275672422 -4.5247932275672369
		4.1671893130569511e-15 6.3990239493595178 4.219451313044492e-15
		4.6789959937687369e-15 4.524793227567236 4.5247932275672422
		;
	setAttr ".adot" yes;
createNode transform -n "upThumb1_L_CTL_GRP" -p "upWrist_L_CTL";
	rename -uid "612E1B8D-4C20-B0DF-A91A-4C9441DF954D";
	setAttr ".t" -type "double3" 2.0168746852511958 3.4734404254248119 0.77937674780391619 ;
	setAttr ".r" -type "double3" 87.918076111310569 -18.829915607318313 59.238179780174484 ;
	setAttr ".s" -type "double3" 0.99999999999999978 1 0.99999999999999978 ;
createNode transform -n "upThumb1_L_CTL" -p "upThumb1_L_CTL_GRP";
	rename -uid "FD48617C-4E88-DA24-979A-D4902D99DA1A";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upThumb1_L_CTLShape" -p "upThumb1_L_CTL";
	rename -uid "703DDF82-469F-5564-415A-9A9B832A5080";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-3.0415373201787155e-15 2.2928238519031816 -2.2928238519031758
		1.2599809036372148e-15 3.2425425874939973 4.6141461123940384e-15
		4.8234194024335735e-15 2.2928238519031794 2.2928238519031847
		5.5613642322978739e-15 -1.0918869233114143e-15 3.2425425874940021
		3.0415373201787155e-15 -2.2928238519031812 2.2928238519031829
		-1.2599809036372154e-15 -3.2425425874939995 2.6175404838289804e-15
		-4.8234194024335735e-15 -2.2928238519031794 -2.2928238519031776
		-5.5613642322978739e-15 8.1779651585055085e-16 -3.242542587493995
		-3.0415373201787155e-15 2.2928238519031816 -2.2928238519031758
		1.2599809036372148e-15 3.2425425874939973 4.6141461123940384e-15
		4.8234194024335735e-15 2.2928238519031794 2.2928238519031847
		;
	setAttr ".adot" yes;
createNode transform -n "upThumb2_L_CTL_GRP" -p "upThumb1_L_CTL";
	rename -uid "68BE8A29-4CDB-6B08-35FA-A8838A86E317";
	setAttr ".t" -type "double3" 3.2939338184001201 -3.5527136788005009e-15 -3.5527136788005009e-15 ;
	setAttr ".r" -type "double3" 1.310553175309555 8.752440058860218 -9.1231090892213782 ;
	setAttr ".s" -type "double3" 1.0000000000000002 1 1.0000000000000002 ;
createNode transform -n "upThumb2_L_CTL" -p "upThumb2_L_CTL_GRP";
	rename -uid "C55A8F1E-4751-5F3C-6996-91A63026C4B2";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upThumb2_L_CTLShape" -p "upThumb2_L_CTL";
	rename -uid "1794769B-4569-9829-9614-A2BE96D79BEE";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		3.6975396441991482e-15 2.3705911312531009 -2.3705911312530854
		1.0269167526733668e-14 3.3525221286595084 1.3616598430837483e-14
		1.4987519326014824e-14 2.3705911312530956 2.3705911312531112
		1.5088648550071206e-14 -2.9899448299719139e-15 3.3525221286595204
		1.0513315071002856e-14 -2.3705911312531005 2.3705911312531067
		3.9416871884683351e-15 -3.3525221286595102 7.8302253017915381e-15
		-7.7666461081281885e-16 -2.3705911312530956 -2.3705911312530898
		-8.7779383486920263e-16 2.7065579101485992e-15 -3.3525221286594991
		3.6975396441991482e-15 2.3705911312531009 -2.3705911312530854
		1.0269167526733668e-14 3.3525221286595084 1.3616598430837483e-14
		1.4987519326014824e-14 2.3705911312530956 2.3705911312531112
		;
	setAttr ".adot" yes;
createNode transform -n "upFingerA1_L_CTL_GRP" -p "upWrist_L_CTL";
	rename -uid "8533B7B8-47EF-0FDA-55D1-1E83727A58AE";
	setAttr ".t" -type "double3" 7.5346388505817288 2.0998136563286565 5.5244697705347789e-13 ;
	setAttr ".r" -type "double3" -0.017218269329093795 0.19819637138343235 -5.728391465616034 ;
createNode transform -n "upFingerA1_L_CTL" -p "upFingerA1_L_CTL_GRP";
	rename -uid "855D9123-490D-37F4-8EF0-81A50752A410";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upFingerA1_L_CTLShape" -p "upFingerA1_L_CTL";
	rename -uid "05093D7D-4770-5D6C-5610-9E988803C98B";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-9.8357993069420639e-15 1.8481438957567555 -1.8481438957567673
		-1.0546446179956694e-14 2.6136701625962564 -3.6010599622882143e-15
		-9.241390895216457e-15 1.84814389575676 1.8481438957567624
		-6.6851171388754999e-15 -3.5289154124767548e-15 2.6136701625962644
		-4.3750554082599399e-15 -1.8481438957567695 1.8481438957567673
		-3.6644085352453083e-15 -2.6136701625962724 3.7028319501138357e-15
		-4.9694638199855475e-15 -1.8481438957567742 -1.8481438957567624
		-7.5257375763265031e-15 -1.0902871457200964e-14 -2.6136701625962644
		-9.8357993069420639e-15 1.8481438957567555 -1.8481438957567673
		-1.0546446179956694e-14 2.6136701625962564 -3.6010599622882143e-15
		-9.241390895216457e-15 1.84814389575676 1.8481438957567624
		;
	setAttr ".adot" yes;
createNode transform -n "upFingerA2_L_CTL_GRP" -p "upFingerA1_L_CTL";
	rename -uid "9A81FF67-4682-D7B5-0869-5DADEC4D02BB";
	setAttr ".t" -type "double3" 2.3108440016582747 2.8421709430404007e-14 8.8817841970012523e-16 ;
	setAttr ".r" -type "double3" 0.23159184795069906 -5.0868598028178891 2.3549046704374557 ;
	setAttr ".s" -type "double3" 0.99999999999999967 0.99999999999999989 0.99999999999999989 ;
createNode transform -n "upFingerA2_L_CTL" -p "upFingerA2_L_CTL_GRP";
	rename -uid "448AAE91-4033-071F-9C5A-08A4E99DBA0D";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upFingerA2_L_CTLShape" -p "upFingerA2_L_CTL";
	rename -uid "22328AA7-480B-4A5B-B496-33A5C4514804";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-6.5165310835555081e-15 1.8736027871633105 -1.8736027871633167
		-7.6115398707448684e-15 2.6496744721063874 2.1982013850388079e-16
		-8.4100748118212839e-15 1.8736027871633107 1.8736027871633176
		-8.4443649683309893e-15 -6.4619552519256906e-15 2.6496744721063954
		-7.6943236316464956e-15 -1.8736027871633243 1.8736027871633185
		-6.5993148444571346e-15 -2.6496744721064034 1.6597106369102391e-15
		-5.8007799033807198e-15 -1.873602787163325 -1.8736027871633159
		-5.7664897468710144e-15 -7.9728750429647124e-15 -2.6496744721063936
		-6.5165310835555081e-15 1.8736027871633105 -1.8736027871633167
		-7.6115398707448684e-15 2.6496744721063874 2.1982013850388079e-16
		-8.4100748118212839e-15 1.8736027871633107 1.8736027871633176
		;
	setAttr ".adot" yes;
createNode transform -n "upFingerB1_L_CTL_GRP" -p "upWrist_L_CTL";
	rename -uid "1415E2F1-4938-CB11-F0CF-DB8D21032FAC";
	setAttr ".t" -type "double3" 7.4786885620873278 -2.0998136563286707 -5.6266102888002933e-13 ;
	setAttr ".r" -type "double3" -0.017218269329094045 0.19819637138343421 -5.7283914656160961 ;
createNode transform -n "upFingerB1_L_CTL" -p "upFingerB1_L_CTL_GRP";
	rename -uid "47C29473-4F5A-1DB7-C4C1-CFA977188538";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upFingerB1_L_CTLShape" -p "upFingerB1_L_CTL";
	rename -uid "4F72B8DD-4299-2E7E-2B31-3F81FFE491E4";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-4.1157683881499055e-15 1.8589343193030454 -1.8589343193030516
		-5.9815510173025076e-15 2.6289301259191689 -4.3661697210967065e-15
		-4.3434221845458878e-15 1.8589343193030532 1.8589343193030468
		-1.6097554319446193e-16 6.1178352192644323e-15 2.628930125919172
		4.1157683881499047e-15 -1.858934319303045 1.8589343193030552
		5.9815510173025116e-15 -2.6289301259191706 8.0212495854237272e-15
		4.3434221845458878e-15 -1.8589343193030532 -1.8589343193030432
		1.6097554319446243e-16 -6.3400572903571706e-15 -2.6289301259191684
		-4.1157683881499055e-15 1.8589343193030454 -1.8589343193030516
		-5.9815510173025076e-15 2.6289301259191689 -4.3661697210967065e-15
		-4.3434221845458878e-15 1.8589343193030532 1.8589343193030468
		;
	setAttr ".adot" yes;
createNode transform -n "upFingerB2_L_CTL_GRP" -p "upFingerB1_L_CTL";
	rename -uid "D854485C-4214-0AB5-0DDC-56B82DFEFC1B";
	setAttr ".t" -type "double3" 2.310844001658289 1.4210854715202004e-14 1.3322676295501878e-15 ;
	setAttr ".r" -type "double3" -0.37790151394615012 -7.9209328221010109 7.7052857247318878 ;
	setAttr ".s" -type "double3" 0.99999999999999978 0.99999999999999978 1 ;
createNode transform -n "upFingerB2_L_CTL" -p "upFingerB2_L_CTL_GRP";
	rename -uid "45443844-40EB-0DB5-8EFB-1C8675DBEC89";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upFingerB2_L_CTLShape" -p "upFingerB2_L_CTL";
	rename -uid "6C73CDDA-49D5-0D20-A928-63A8FC3FB3DF";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-8.7443780618751065e-16 1.61824218004158 -1.6182421800415974
		-5.3362547818465654e-15 2.288540038219018 -1.2581814801073845e-14
		-2.5099030989523441e-15 1.6182421800415974 1.6182421800415796
		5.9489787586917437e-15 1.2560320459154732e-14 2.2885400382190184
		1.5085292521389516e-14 -1.6182421800415798 1.6182421800415974
		1.9547109497048577e-14 -2.2885400382190193 1.2670926760280178e-14
		1.6720757814154348e-14 -1.6182421800415974 -1.6182421800415796
		8.2618759565102616e-15 -1.2753769536426756e-14 -2.2885400382190184
		-8.7443780618751065e-16 1.61824218004158 -1.6182421800415974
		-5.3362547818465654e-15 2.288540038219018 -1.2581814801073845e-14
		-2.5099030989523441e-15 1.6182421800415974 1.6182421800415796
		;
	setAttr ".adot" yes;
createNode transform -n "upShoulder_R_CTL_GRP" -p "head_CTL";
	rename -uid "F3598243-442E-9168-04AB-C2855DB0115E";
	setAttr ".t" -type "double3" 7.5 -5.8 -16 ;
	setAttr ".r" -type "double3" 49.28995741799514 -84.982002891423036 40.60129464500438 ;
createNode transform -n "upShoulder_R_CTL" -p "upShoulder_R_CTL_GRP";
	rename -uid "4C965D24-42BF-8C71-92DB-CCA54F9FBEBA";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upShoulder_R_CTLShape" -p "upShoulder_R_CTL";
	rename -uid "B462BAE0-43AD-9BC4-7725-7C880ECAC7BE";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-3.500000000000004 5.0053666945844224 5.0053666945844366
		-3.5000000000000049 7.0786574641318856 6.2961186344454478e-15
		-3.5000000000000022 5.0053666945844304 -5.0053666945844295
		-3.4999999999999969 1.2385789196927922e-17 -7.0786574641318971
		-3.4999999999999925 -5.0053666945844357 -5.0053666945844384
		-3.4999999999999916 -7.0786574641319042 -8.3481066730578289e-15
		-3.4999999999999942 -5.0053666945844446 5.0053666945844277
		-3.4999999999999996 -1.4821595670527765e-14 7.0786574641318953
		-3.500000000000004 5.0053666945844224 5.0053666945844366
		-3.5000000000000049 7.0786574641318856 6.2961186344454478e-15
		-3.5000000000000022 5.0053666945844304 -5.0053666945844295
		;
	setAttr ".adot" yes;
createNode transform -n "upElbow_R_CTL_GRP" -p "upShoulder_R_CTL";
	rename -uid "6F0D87A0-4169-B26D-8D2F-2F9E99FE7F75";
	setAttr ".t" -type "double3" -10.540398474441082 -7.1054273576010019e-15 -3.1086244689504383e-15 ;
	setAttr ".r" -type "double3" -0.078774276723312761 -3.8543147501225095 4.9796103462861456 ;
	setAttr ".s" -type "double3" 0.99999999999999989 0.99999999999999989 0.99999999999999978 ;
createNode transform -n "upElbow_R_CTL" -p "upElbow_R_CTL_GRP";
	rename -uid "46C78D3E-4DB5-A23A-7EA8-56B60136EF47";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upElbow_R_CTLShape" -p "upElbow_R_CTL";
	rename -uid "99177E65-47E2-BAFE-2BB9-7AB4BCD2EF5F";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-3.964007623184196e-15 4.7087140365722382 4.7087140365722417
		-1.5848785372067245e-15 6.6591272518570186 -6.7190277818856949e-16
		-3.5848058866050605e-16 4.7087140365722338 -4.7087140365722435
		-1.0032210629373758e-15 -8.7280521665447576e-15 -6.6591272518570275
		-3.1414197344168055e-15 -4.7087140365722515 -4.70871403657224
		-5.5205488203942778e-15 -6.6591272518570364 2.1889642300897179e-15
		-6.7469467689404954e-15 -4.708714036572248 4.7087140365722453
		-6.1022062946636266e-15 -6.0456950481527857e-15 6.6591272518570293
		-3.964007623184196e-15 4.7087140365722382 4.7087140365722417
		-1.5848785372067245e-15 6.6591272518570186 -6.7190277818856949e-16
		-3.5848058866050605e-16 4.7087140365722338 -4.7087140365722435
		;
	setAttr ".adot" yes;
createNode transform -n "upWrist_R_CTL_GRP" -p "upElbow_R_CTL";
	rename -uid "918CE0AC-4465-313F-0989-5FB9775D9001";
	setAttr ".t" -type "double3" -9.8025506884688127 -1.4210854715202004e-14 -1.7763568394002505e-15 ;
	setAttr ".r" -type "double3" 0.038334688090915489 2.8749852142838219 -0.40482835540026552 ;
	setAttr ".s" -type "double3" 1.0000000000000002 1 1.0000000000000002 ;
createNode transform -n "upWrist_R_CTL" -p "upWrist_R_CTL_GRP";
	rename -uid "A28B974C-43DC-08E1-7FA5-1483F84AC943";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upWrist_R_CTLShape" -p "upWrist_R_CTL";
	rename -uid "043B9244-4B9A-6C29-9D2F-1A8A7EF3CFDA";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		5.7105487616273955e-15 4.5247932275671943 4.5247932275671872
		4.8423708079411571e-15 6.3990239493594476 -4.0064543882285823e-15
		1.1375977090025542e-15 4.5247932275671854 -4.5247932275671925
		-3.2335646993451843e-15 -4.5106442764234146e-15 -6.3990239493594494
		-5.7105487616273939e-15 -4.5247932275671934 -4.5247932275671854
		-4.8423708079411611e-15 -6.3990239493594521 4.6454654122910187e-15
		-1.1375977090025542e-15 -4.5247932275671854 4.5247932275671943
		3.2335646993451839e-15 3.9697381713204846e-15 6.3990239493594512
		5.7105487616273955e-15 4.5247932275671943 4.5247932275671872
		4.8423708079411571e-15 6.3990239493594476 -4.0064543882285823e-15
		1.1375977090025542e-15 4.5247932275671854 -4.5247932275671925
		;
	setAttr ".adot" yes;
createNode transform -n "upThumb1_R_CTL_GRP" -p "upWrist_R_CTL";
	rename -uid "6AC13F4F-4D4C-02B2-E4B8-279B2FB63D56";
	setAttr ".t" -type "double3" -2.0168746852511887 -3.4734404254254798 -0.77937674780112198 ;
	setAttr ".r" -type "double3" 87.918076111285885 -18.829915607279059 59.238179780182463 ;
	setAttr ".s" -type "double3" 1.0000000000000002 1.0000000000000002 1 ;
createNode transform -n "upThumb1_R_CTL" -p "upThumb1_R_CTL_GRP";
	rename -uid "604D3454-49F0-FB11-C603-3CB1485199CC";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upThumb1_R_CTLShape" -p "upThumb1_R_CTL";
	rename -uid "5E200C5D-4693-3936-42C8-9C84FBBA86EB";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-5.9490304418343129e-13 2.2928238519031852 2.2928238519042083
		-7.1558361951450758e-13 3.2425425874947282 7.2111123850272309e-13
		-4.1292275256638818e-13 2.2928238519042066 -2.2928238519031896
		1.3578492560231748e-13 7.1930442717363416e-13 -3.242542587494734
		6.0911389889863329e-13 -2.2928238519031918 -2.2928238519042119
		7.2979447422970989e-13 -3.2425425874947371 -7.2479021142014601e-13
		4.2713360728159018e-13 -2.2928238519042137 2.292823851903186
		-1.215740708871154e-13 -7.2668394493869605e-13 3.2425425874947305
		-5.9490304418343129e-13 2.2928238519031852 2.2928238519042083
		-7.1558361951450758e-13 3.2425425874947282 7.2111123850272309e-13
		-4.1292275256638818e-13 2.2928238519042066 -2.2928238519031896
		;
	setAttr ".adot" yes;
createNode transform -n "upThumb2_R_CTL_GRP" -p "upThumb1_R_CTL";
	rename -uid "66C9DEC3-4EA4-69BC-57F3-918A4ABB2754";
	setAttr ".t" -type "double3" -3.2939338184001201 -7.3896444519050419e-13 -1.4566126083082054e-13 ;
	setAttr ".r" -type "double3" 1.310553175309557 8.7524400588602003 -9.1231090892213889 ;
	setAttr ".s" -type "double3" 0.99999999999999967 0.99999999999999967 0.99999999999999989 ;
createNode transform -n "upThumb2_R_CTL" -p "upThumb2_R_CTL_GRP";
	rename -uid "AAD177A9-4DCE-CCBB-50A3-1BA0FF6A5EA5";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upThumb2_R_CTLShape" -p "upThumb2_R_CTL";
	rename -uid "3424985B-4E49-A9BA-29A9-E2A01D4BCA9D";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-5.94200240739214e-13 2.3705911312532075 2.3705911312543355
		-7.9735557380311188e-13 3.3525221286604596 7.9756085657779857e-13
		-5.3343082576692866e-13 2.3705911312543346 -2.3705911312532075
		4.297046541564215e-14 7.9752936914227301e-13 -3.3525221286604614
		5.942002407392139e-13 -2.3705911312532071 -2.3705911312543355
		7.9735557380311238e-13 -3.3525221286604618 -7.9769139823762507e-13
		5.3343082576692866e-13 -2.3705911312543346 2.3705911312532075
		-4.2970465415642087e-14 -7.978127560620964e-13 3.3525221286604614
		-5.94200240739214e-13 2.3705911312532075 2.3705911312543355
		-7.9735557380311188e-13 3.3525221286604596 7.9756085657779857e-13
		-5.3343082576692866e-13 2.3705911312543346 -2.3705911312532075
		;
	setAttr ".adot" yes;
createNode transform -n "upFingerA1_R_CTL_GRP" -p "upWrist_R_CTL";
	rename -uid "057F5538-4D29-16F3-9ABF-FDA7A5FFC423";
	setAttr ".t" -type "double3" -7.5346388505817217 -2.0998136563286778 1.1355361095866101e-12 ;
	setAttr ".r" -type "double3" -0.017218269628334375 0.19819637137887286 -5.7283914656161867 ;
createNode transform -n "upFingerA1_R_CTL" -p "upFingerA1_R_CTL_GRP";
	rename -uid "40314457-4072-D835-2F97-8A8B95C7043B";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upFingerA1_R_CTLShape" -p "upFingerA1_R_CTL";
	rename -uid "06437010-4AA4-D19B-020F-399602EEDED1";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		3.6288515992657779e-14 1.8481438957566394 1.8481438957566667
		-1.2720386701787667e-14 2.6136701625961054 1.954176598947159e-14
		-5.0115596405330443e-14 1.8481438957566669 -1.8481438957566396
		-5.3991506441421116e-14 1.9961307182157228e-14 -2.6136701625961072
		-2.2077661277455782e-14 -1.8481438957566392 -1.8481438957566676
		2.6931241416989681e-14 -2.6136701625961072 -2.0531716396997342e-14
		6.4326451120532447e-14 -1.8481438957566669 1.8481438957566387
		6.820236115662312e-14 -2.0182239336632927e-14 2.6136701625961063
		3.6288515992657779e-14 1.8481438957566394 1.8481438957566667
		-1.2720386701787667e-14 2.6136701625961054 1.954176598947159e-14
		-5.0115596405330443e-14 1.8481438957566669 -1.8481438957566396
		;
	setAttr ".adot" yes;
createNode transform -n "upFingerA2_R_CTL_GRP" -p "upFingerA1_R_CTL";
	rename -uid "889F9AC5-4874-641E-7644-57B4003C7C15";
	setAttr ".t" -type "double3" -2.3108440016582747 -1.4210854715202004e-14 5.3290705182007514e-14 ;
	setAttr ".r" -type "double3" 0.2315918482084047 -5.0868598028283216 2.3549046704148737 ;
	setAttr ".s" -type "double3" 0.99999999999999978 1.0000000000000002 0.99999999999999967 ;
createNode transform -n "upFingerA2_R_CTL" -p "upFingerA2_R_CTL_GRP";
	rename -uid "692FEC80-446F-A48E-D88F-BE9C26C59371";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upFingerA2_R_CTLShape" -p "upFingerA2_R_CTL";
	rename -uid "A5F7629A-4B82-66AF-300C-0EB4C3F8C4C4";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-1.7658742668507216e-13 1.8736027871631458 1.8736027871633134
		-1.1928178499344261e-13 2.6496744721062702 1.1855585234178896e-13
		7.8975086032740223e-15 1.8736027871633139 -1.8736027871631464
		1.3045054876915094e-13 1.194191445859741e-13 -2.6496744721062719
		1.7658742668507213e-13 -1.8736027871631455 -1.8736027871633152
		1.1928178499344271e-13 -2.6496744721062719 -1.2043538311720317e-13
		-7.8975086032740223e-15 -1.8736027871633139 1.8736027871631447
		-1.3045054876915091e-13 -1.1964312016566248e-13 2.6496744721062702
		-1.7658742668507216e-13 1.8736027871631458 1.8736027871633134
		-1.1928178499344261e-13 2.6496744721062702 1.1855585234178896e-13
		7.8975086032740223e-15 1.8736027871633139 -1.8736027871631464
		;
	setAttr ".adot" yes;
createNode transform -n "upFingerB1_R_CTL_GRP" -p "upWrist_R_CTL";
	rename -uid "CEAC2820-4BBE-B3AB-A508-F09084CC19BB";
	setAttr ".t" -type "double3" -7.4786885620872994 2.0998136563286494 -1.1315393066979595e-12 ;
	setAttr ".r" -type "double3" -0.017218269678245923 0.19819637137887447 -5.7283914656162453 ;
	setAttr ".s" -type "double3" 1 1.0000000000000002 1 ;
createNode transform -n "upFingerB1_R_CTL" -p "upFingerB1_R_CTL_GRP";
	rename -uid "96FBC593-4B74-524E-48FC-ABB0F43FF954";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upFingerB1_R_CTLShape" -p "upFingerB1_R_CTL";
	rename -uid "8D18C4BC-4E33-F102-7EF9-70967FB3E3D5";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		2.6507320243696901e-14 1.8589343193030694 1.8589343193030883
		-2.0439733497681683e-14 2.6289301259192102 1.3939370893125508e-14
		-5.9575731547035849e-14 1.8589343193030876 -1.8589343193030687
		-6.7975337024060906e-14 1.3470590342042616e-14 -2.628930125919211
		-4.0718174958898904e-14 -1.8589343193030692 -1.8589343193030874
		6.228878782479687e-15 -2.6289301259192119 -1.3153558658951911e-14
		4.5364876831833845e-14 -1.8589343193030876 1.8589343193030696
		5.3764482308858902e-14 -1.3692812413135357e-14 2.6289301259192119
		2.6507320243696901e-14 1.8589343193030694 1.8589343193030883
		-2.0439733497681683e-14 2.6289301259192102 1.3939370893125508e-14
		-5.9575731547035849e-14 1.8589343193030876 -1.8589343193030687
		;
	setAttr ".adot" yes;
createNode transform -n "upFingerB2_R_CTL_GRP" -p "upFingerB1_R_CTL";
	rename -uid "E7717E3A-4FE3-249B-2F62-F5B6DC01F799";
	setAttr ".t" -type "double3" -2.3108440016582534 -7.1054273576010019e-15 5.2402526762307389e-14 ;
	setAttr ".r" -type "double3" -0.37790151362899405 -7.9209328221417286 7.7052857246900004 ;
	setAttr ".s" -type "double3" 0.99999999999999967 0.99999999999999967 0.99999999999999978 ;
createNode transform -n "upFingerB2_R_CTL" -p "upFingerB2_R_CTL_GRP";
	rename -uid "CE4E1732-4089-588F-D096-638A926B0797";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "upFingerB2_R_CTLShape" -p "upFingerB2_R_CTL";
	rename -uid "566D856E-4F11-F623-F533-428576DE59CF";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-8.3416197832087538e-14 1.6182421800415794 1.6182421800419287
		-2.3640347421698542e-13 2.2885400382192493 2.4542539107562528e-13
		-2.4674653861806582e-13 1.6182421800419231 -1.6182421800415809
		-1.0838656418567405e-13 2.400748262154932e-13 -2.288540038219252
		9.7627052547289517e-14 -1.6182421800415863 -1.6182421800419251
		2.5061432893218757e-13 -2.2885400382192578 -2.4196178935603126e-13
		2.6095739333326782e-13 -1.6182421800419302 1.6182421800415845
		1.2259741890087605e-13 -2.4737370265036625e-13 2.2885400382192556
		-8.3416197832087538e-14 1.6182421800415794 1.6182421800419287
		-2.3640347421698542e-13 2.2885400382192493 2.4542539107562528e-13
		-2.4674653861806582e-13 1.6182421800419231 -1.6182421800415809
		;
	setAttr ".adot" yes;
createNode transform -n "eyes_CTL_GRP" -p "head_CTL";
	rename -uid "DC809BBE-418E-3606-0498-AAAF83D85528";
	setAttr ".t" -type "double3" 10.174 44 0 ;
	setAttr ".r" -type "double3" -90 -90 0 ;
createNode transform -n "eyes_CTL" -p "eyes_CTL_GRP";
	rename -uid "6774A7F2-4325-8592-3338-5F875A158169";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "eyes_CTLShape" -p "eyes_CTL";
	rename -uid "AE8C9B9E-4FCB-7CD8-6287-7196A373F88D";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 17;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		1 4 0 no 3
		5 0 1 2 3 4
		5
		-22.132485580294212 -9.9596185111323958 0
		22.132485580294212 -9.9596185111323958 0
		22.132485580294212 9.9596185111323958 0
		-22.132485580294212 9.9596185111323958 0
		-22.132485580294212 -9.9596185111323958 0
		;
	setAttr ".adot" yes;
createNode transform -n "eye_L_CTL_GRP" -p "eyes_CTL";
	rename -uid "5E3887BB-4750-1815-E166-0F887C48400A";
	setAttr ".t" -type "double3" 17.132485580294212 0 0 ;
createNode transform -n "eye_L_CTL" -p "eye_L_CTL_GRP";
	rename -uid "F6A831A9-4F90-A272-7839-FD9BF065911E";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".rx";
	setAttr -l on -k off ".ry";
	setAttr -l on -k off ".rz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "eye_L_CTLShape" -p "eye_L_CTL";
	rename -uid "5621F08E-4D1D-8AD7-FED9-F4B9C9B26F47";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.9590290622280613 1.9590290622280615 0
		1.696433080777728e-16 2.770485468885969 0
		-1.9590290622280613 1.9590290622280611 0
		-2.7704854688859704 1.4362245593812077e-16 0
		-1.9590290622280613 -1.9590290622280613 0
		-2.7752142424008064e-16 -2.7704854688859708 0
		1.9590290622280613 -1.9590290622280611 0
		2.7704854688859704 -3.7781012519498975e-16 0
		1.9590290622280613 1.9590290622280615 0
		1.696433080777728e-16 2.770485468885969 0
		-1.9590290622280613 1.9590290622280611 0
		;
	setAttr ".adot" yes;
createNode transform -n "eye_R_CTL_GRP" -p "eyes_CTL";
	rename -uid "5646B918-4F1D-5CF5-E0BE-5FAF08C77282";
	setAttr ".t" -type "double3" -17.132485580294212 0 0 ;
createNode transform -n "eye_R_CTL" -p "eye_R_CTL_GRP";
	rename -uid "99CB191A-48CD-C91D-8AD1-418DD35CA7BC";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".rx";
	setAttr -l on -k off ".ry";
	setAttr -l on -k off ".rz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "eye_R_CTLShape" -p "eye_R_CTL";
	rename -uid "69356FB2-437F-171C-2F11-ED8A5E00B51F";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.9590290622280613 1.9590290622280615 0
		1.696433080777728e-16 2.770485468885969 0
		-1.9590290622280613 1.9590290622280611 0
		-2.7704854688859704 1.4362245593812077e-16 0
		-1.9590290622280613 -1.9590290622280613 0
		-2.7752142424008064e-16 -2.7704854688859708 0
		1.9590290622280613 -1.9590290622280611 0
		2.7704854688859704 -3.7781012519498975e-16 0
		1.9590290622280613 1.9590290622280615 0
		1.696433080777728e-16 2.770485468885969 0
		-1.9590290622280613 1.9590290622280611 0
		;
	setAttr ".adot" yes;
createNode transform -n "loShoulder_L_CTL_GRP" -p "spine_CTL";
	rename -uid "B936A241-42F2-143C-DAAE-8385854153B3";
	setAttr ".t" -type "double3" 0.5 -2 16.5 ;
	setAttr ".r" -type "double3" 49.207034055317635 -83.555512044930111 -139.38659336288899 ;
createNode transform -n "loShoulder_L_CTL" -p "loShoulder_L_CTL_GRP";
	rename -uid "F85C328F-4F5B-B9B7-FD18-65B86D5ED5A4";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loShoulder_L_CTLShape" -p "loShoulder_L_CTL";
	rename -uid "6571D0A7-452C-5D6E-973E-5F8BBA656BF0";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		3.5000000000000027 4.805050493017446 -4.8050504930174398
		3.5000000000000004 6.7953675751127944 8.3899965213176396e-16
		3.4999999999999978 4.8050504930174442 4.8050504930174416
		3.4999999999999964 3.6213359707870076e-15 6.7953675751127935
		3.4999999999999973 -4.805050493017438 4.8050504930174416
		3.4999999999999996 -6.7953675751127918 1.3684910010314178e-15
		3.5000000000000022 -4.8050504930174371 -4.8050504930174398
		3.5000000000000036 2.90968256495532e-15 -6.7953675751127918
		3.5000000000000027 4.805050493017446 -4.8050504930174398
		3.5000000000000004 6.7953675751127944 8.3899965213176396e-16
		3.4999999999999978 4.8050504930174442 4.8050504930174416
		;
	setAttr ".adot" yes;
createNode transform -n "loElbow_L_CTL_GRP" -p "loShoulder_L_CTL";
	rename -uid "511A4618-4292-476D-4F50-779E19EDD8F7";
	setAttr ".t" -type "double3" 9.1427725554122841 7.1054273576010019e-15 1.0824674490095276e-15 ;
	setAttr ".r" -type "double3" 0.097090872592850611 -4.2245823943083636 3.5699958052975291 ;
	setAttr ".s" -type "double3" 1 0.99999999999999978 0.99999999999999989 ;
createNode transform -n "loElbow_L_CTL" -p "loElbow_L_CTL_GRP";
	rename -uid "0C23212A-41AD-0A08-E07B-A99E53595C50";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loElbow_L_CTLShape" -p "loElbow_L_CTL";
	rename -uid "F0E08582-43F2-42D0-226E-5092EBAC7CE6";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		4.9770160280347697e-15 4.9177182419927785 -4.917718241992775
		3.5242018472699027e-15 6.9547038337557554 3.0983490578185627e-15
		6.9580209146431793e-18 4.9177182419927714 4.9177182419927803
		-3.514361719725139e-15 -6.7163824512198401e-15 6.9547038337557607
		-4.9770160280347697e-15 -4.9177182419927847 4.917718241992775
		-3.5242018472699051e-15 -6.954703833755767 -2.8275444077028451e-15
		-6.9580209146431793e-18 -4.9177182419927785 -4.9177182419927803
		3.5143617197251386e-15 -9.7692233778815125e-16 -6.9547038337557607
		4.9770160280347697e-15 4.9177182419927785 -4.917718241992775
		3.5242018472699027e-15 6.9547038337557554 3.0983490578185627e-15
		6.9580209146431793e-18 4.9177182419927714 4.9177182419927803
		;
	setAttr ".adot" yes;
createNode transform -n "loWrist_L_CTL_GRP" -p "loElbow_L_CTL";
	rename -uid "39C9D126-49B5-417C-BACF-3292C296DC99";
	setAttr ".t" -type "double3" 8.415214019857121 -1.0658141036401503e-14 2.2204460492503131e-15 ;
	setAttr ".r" -type "double3" 0.3337809332185378 -0.88979687962355325 -19.248887884355749 ;
	setAttr ".s" -type "double3" 0.99999999999999989 1.0000000000000004 1.0000000000000004 ;
createNode transform -n "loWrist_L_CTL" -p "loWrist_L_CTL_GRP";
	rename -uid "3AFFB8F1-47CA-60BD-C97E-4D871CD940D6";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loWrist_L_CTLShape" -p "loWrist_L_CTL";
	rename -uid "3417C27E-41C2-08E3-9C09-9D9D01B115B1";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.890317703194484e-14 3.8096974388567175 -3.8096974388567282
		-9.2109702928749008e-15 5.3877257865692192 -5.5440704611846609e-15
		-3.4010587632557129e-14 3.8096974388567246 3.8096974388567211
		-4.0968395491178973e-14 2.3848435914507306e-15 5.3877257865692254
		-2.6008604389545842e-14 -3.8096974388567242 3.8096974388567291
		2.1055429352739005e-15 -5.3877257865692298 6.6420380007995347e-15
		2.6905160274956127e-14 -3.8096974388567317 -3.8096974388567202
		3.3862968133577971e-14 -9.9456925561028351e-15 -5.3877257865692254
		1.890317703194484e-14 3.8096974388567175 -3.8096974388567282
		-9.2109702928749008e-15 5.3877257865692192 -5.5440704611846609e-15
		-3.4010587632557129e-14 3.8096974388567246 3.8096974388567211
		;
	setAttr ".adot" yes;
createNode transform -n "loThumb1_L_CTL_GRP" -p "loWrist_L_CTL";
	rename -uid "B096DAA8-4C9A-673E-8699-B09DE4D23999";
	setAttr ".t" -type "double3" 0.42272365381577615 3.6617016049175 0.97564700022563877 ;
	setAttr ".r" -type "double3" 90.868851202962745 -12.887106561821964 81.111154518829238 ;
	setAttr ".s" -type "double3" 0.99999999999999989 0.99999999999999989 0.99999999999999978 ;
createNode transform -n "loThumb1_L_CTL" -p "loThumb1_L_CTL_GRP";
	rename -uid "FDFE5897-497F-F24C-0A69-4E81DEABD8FA";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loThumb1_L_CTLShape" -p "loThumb1_L_CTL";
	rename -uid "644F861B-4D59-837B-56DA-41A521351BAB";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-1.286983946359627e-14 2.2142686752307998 -2.2142686752307998
		-9.8867125963846816e-15 3.1314487912493045 5.7968250307196156e-16
		-5.2743465571217061e-15 2.214268675230803 2.2142686752308034
		-1.7346028169785181e-15 2.9436200970944623e-15 3.1314487912493094
		-1.3410152516057334e-15 -2.2142686752307998 2.2142686752308069
		-4.3241421188173205e-15 -3.1314487912493063 6.6476782846995129e-15
		-8.9365081580802976e-15 -2.214268675230803 -2.2142686752307963
		-1.2476251898223486e-14 -3.2083198048012123e-15 -3.1314487912493023
		-1.286983946359627e-14 2.2142686752307998 -2.2142686752307998
		-9.8867125963846816e-15 3.1314487912493045 5.7968250307196156e-16
		-5.2743465571217061e-15 2.214268675230803 2.2142686752308034
		;
	setAttr ".adot" yes;
createNode transform -n "loThumb2_L_CTL_GRP" -p "loThumb1_L_CTL";
	rename -uid "B5A9D966-4D79-EA41-490F-3F94981F2F1A";
	setAttr ".t" -type "double3" 2.5787619122361818 1.7763568394002505e-15 -3.5527136788005009e-15 ;
	setAttr ".r" -type "double3" 0.92887510270752949 16.780262712054888 -10.148617095669948 ;
	setAttr ".s" -type "double3" 0.99999999999999978 0.99999999999999978 1.0000000000000002 ;
createNode transform -n "loThumb2_L_CTL" -p "loThumb2_L_CTL_GRP";
	rename -uid "38D4E947-4E2E-85D7-93A6-A39D513692F4";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loThumb2_L_CTLShape" -p "loThumb2_L_CTL";
	rename -uid "5B585DE4-470D-98BB-2F23-B1BB78BF594C";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-2.2200986599019341e-15 2.1888070040539764 -2.1888070040539795
		1.0739466783060537e-16 3.0954405505503542 -3.634860079079016e-15
		2.3719776556745301e-15 2.1888070040539755 2.1888070040539724
		3.2470883024702544e-15 5.3073516923764164e-17 3.0954405505503515
		2.2200986599019341e-15 -2.188807004053976 2.1888070040539724
		-1.0739466783060531e-16 -3.0954405505503559 -3.3500359496137479e-15
		-2.3719776556745301e-15 -2.1888070040539755 -2.1888070040539795
		-3.2470883024702544e-15 -3.1472946711652208e-16 -3.0954405505503586
		-2.2200986599019341e-15 2.1888070040539764 -2.1888070040539795
		1.0739466783060537e-16 3.0954405505503542 -3.634860079079016e-15
		2.3719776556745301e-15 2.1888070040539755 2.1888070040539724
		;
	setAttr ".adot" yes;
createNode transform -n "loFingerA1_L_CTL_GRP" -p "loWrist_L_CTL";
	rename -uid "3D93DC54-41C1-76E2-3956-B6905729C48F";
	setAttr ".t" -type "double3" 5.5213954127458607 1.7559403927372195 -0.086046692341986919 ;
	setAttr ".r" -type "double3" 0.10759895187179151 15.754415943656394 20.956755684056056 ;
	setAttr ".s" -type "double3" 0.99999999999999989 0.99999999999999967 0.99999999999999978 ;
createNode transform -n "loFingerA1_L_CTL" -p "loFingerA1_L_CTL_GRP";
	rename -uid "15BC2FAA-4C82-8C32-6969-648077618B86";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loFingerA1_L_CTLShape" -p "loFingerA1_L_CTL";
	rename -uid "99A7E5B9-4857-D977-9530-0FA64B5A5BDC";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		4.1705679047628154e-15 1.621338833065896 -1.6213388330658869
		6.2668032504967585e-15 2.2929193669239503 6.1264024323264375e-15
		4.6920302448134997e-15 1.6213388330658867 1.6213388330658958
		3.6872955678324528e-16 -6.1479379056162051e-15 2.2929193669239512
		-4.1705679047628154e-15 -1.6213388330658958 1.6213388330658869
		-6.2668032504967625e-15 -2.2929193669239516 -6.0371199493126274e-15
		-4.6920302448134997e-15 -1.6213388330658867 -1.6213388330658958
		-3.6872955678324577e-16 5.9541186460083299e-15 -2.2929193669239512
		4.1705679047628154e-15 1.621338833065896 -1.6213388330658869
		6.2668032504967585e-15 2.2929193669239503 6.1264024323264375e-15
		4.6920302448134997e-15 1.6213388330658867 1.6213388330658958
		;
	setAttr ".adot" yes;
createNode transform -n "loFingerA2_L_CTL_GRP" -p "loFingerA1_L_CTL";
	rename -uid "7C84757D-4B36-0BED-279B-CB9EA116548F";
	setAttr ".t" -type "double3" 1.8027182253474834 -1.0658141036401503e-14 -1.7763568394002505e-15 ;
	setAttr ".r" -type "double3" 0.5198238385415842 6.8774812014232518 3.9514721757772997 ;
	setAttr ".s" -type "double3" 0.99999999999999989 1 0.99999999999999989 ;
createNode transform -n "loFingerA2_L_CTL" -p "loFingerA2_L_CTL_GRP";
	rename -uid "DBA107D9-4B79-0B11-F3D8-678847035867";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loFingerA2_L_CTLShape" -p "loFingerA2_L_CTL";
	rename -uid "1AFE7B4F-40CF-2C54-C39F-8C8AEDFD9A8B";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-4.4083092327484697e-15 1.6419293401550312 -1.6419293401550346
		-1.2205128203511288e-15 2.322038741305553 -1.3626966859529257e-15
		2.6822434491576663e-15 1.6419293401550326 1.6419293401550328
		5.0137778837362902e-15 -4.3546912065852719e-16 2.3220387413055557
		4.4083092327484697e-15 -1.6419293401550346 1.6419293401550346
		1.2205128203511298e-15 -2.3220387413055579 1.4531130291827163e-15
		-2.6822434491576663e-15 -1.6419293401550361 -1.6419293401550328
		-5.0137778837362902e-15 -3.3135252630428243e-15 -2.3220387413055557
		-4.4083092327484697e-15 1.6419293401550312 -1.6419293401550346
		-1.2205128203511288e-15 2.322038741305553 -1.3626966859529257e-15
		2.6822434491576663e-15 1.6419293401550326 1.6419293401550328
		;
	setAttr ".adot" yes;
createNode transform -n "loFingerB1_L_CTL_GRP" -p "loWrist_L_CTL";
	rename -uid "2AF399E8-4CC9-17C5-2837-B0BC9A4F71EB";
	setAttr ".t" -type "double3" 6.708251434158619 -1.7559403927371875 0.086046692342074849 ;
	setAttr ".r" -type "double3" -2.8335492771707815 15.298551737982526 9.9358856464689005 ;
	setAttr ".s" -type "double3" 0.99999999999999978 0.99999999999999978 0.99999999999999967 ;
createNode transform -n "loFingerB1_L_CTL" -p "loFingerB1_L_CTL_GRP";
	rename -uid "0F6BC0F3-4BF7-B1BE-FF50-A9A5BA171E91";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loFingerB1_L_CTLShape" -p "loFingerB1_L_CTL";
	rename -uid "AD6570F7-49C4-BA2A-CB3C-F6A6A01A19B7";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-5.0428573354207198e-15 1.4801363956320501 -1.4801363956320428
		-3.3871057694325912e-15 2.0932289648648683 1.8137914411829446e-15
		-3.9094965605288499e-15 1.4801363956320444 1.4801363956320446
		-6.3040202681441197e-15 -5.7094550192781311e-17 2.0932289648648639
		-9.1679973797812847e-15 -1.4801363956320428 1.4801363956320392
		-1.0823748945769415e-14 -2.0932289648648625 -5.2849982505333078e-15
		-1.0301358154673153e-14 -1.4801363956320372 -1.4801363956320481
		-7.9068344470578848e-15 6.9855823721030264e-15 -2.0932289648648674
		-5.0428573354207198e-15 1.4801363956320501 -1.4801363956320428
		-3.3871057694325912e-15 2.0932289648648683 1.8137914411829446e-15
		-3.9094965605288499e-15 1.4801363956320444 1.4801363956320446
		;
	setAttr ".adot" yes;
createNode transform -n "loFingerB2_L_CTL_GRP" -p "loFingerB1_L_CTL";
	rename -uid "F21A25A3-4A7A-AF47-936A-3088BB2CCC81";
	setAttr ".t" -type "double3" 1.6306823725054187 -3.5527136788005009e-15 -7.1054273576010019e-15 ;
	setAttr ".r" -type "double3" 1.3643449526125007 -5.6302836786381985 -3.4011530161809391 ;
	setAttr ".s" -type "double3" 1.0000000000000002 0.99999999999999978 1 ;
createNode transform -n "loFingerB2_L_CTL" -p "loFingerB2_L_CTL_GRP";
	rename -uid "3BD3180C-4A92-7731-0786-C2B4F5D91D24";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loFingerB2_L_CTLShape" -p "loFingerB2_L_CTL";
	rename -uid "4D249E86-4FD1-1E9C-1F02-51874F4E5FD3";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		2.9635934957380818e-15 1.5215517609228264 -1.5215517609228328
		-1.6741502499638962e-15 2.1517991361497284 -2.6940883658887171e-15
		-5.3311994846873322e-15 1.5215517609228282 1.5215517609228288
		-5.8653043649973834e-15 -1.7670137816393492e-15 2.1517991361497315
		-2.9635934957380818e-15 -1.5215517609228333 1.5215517609228311
		1.6741502499638967e-15 -2.1517991361497364 1.0015190214147107e-15
		5.3311994846873322e-15 -1.5215517609228353 -1.5215517609228306
		5.8653043649973834e-15 -5.5203040172085733e-15 -2.1517991361497333
		2.9635934957380818e-15 1.5215517609228264 -1.5215517609228328
		-1.6741502499638962e-15 2.1517991361497284 -2.6940883658887171e-15
		-5.3311994846873322e-15 1.5215517609228282 1.5215517609228288
		;
	setAttr ".adot" yes;
createNode transform -n "loShoulder_R_CTL_GRP" -p "spine_CTL";
	rename -uid "68BD9BDD-4BC8-7720-520F-43BF3E426FAB";
	setAttr ".t" -type "double3" 0.5 -2 -16.5 ;
	setAttr ".r" -type "double3" 49.207034055317493 -83.555512044930111 40.613406637111126 ;
	setAttr ".s" -type "double3" 0.99999999999999989 1 0.99999999999999989 ;
createNode transform -n "loShoulder_R_CTL" -p "loShoulder_R_CTL_GRP";
	rename -uid "581E4A68-4527-71DB-7AAC-328B67BC3273";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loShoulder_R_CTLShape" -p "loShoulder_R_CTL";
	rename -uid "C390338F-474C-0987-FF25-5E81B7B59214";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-3.5000000000000031 4.8050504930174389 4.8050504930174389
		-3.5000000000000027 6.7953675751127873 3.0962852152239538e-16
		-3.5000000000000013 4.8050504930174398 -4.8050504930174389
		-3.5000000000000009 1.0507170119468929e-15 -6.7953675751127909
		-3.5000000000000004 -4.805050493017438 -4.8050504930174407
		-3.5000000000000009 -6.7953675751127918 -2.1840522672980307e-15
		-3.5000000000000022 -4.8050504930174398 4.8050504930174371
		-3.5000000000000027 -1.6251258338055668e-15 6.7953675751127891
		-3.5000000000000031 4.8050504930174389 4.8050504930174389
		-3.5000000000000027 6.7953675751127873 3.0962852152239538e-16
		-3.5000000000000013 4.8050504930174398 -4.8050504930174389
		;
	setAttr ".adot" yes;
createNode transform -n "loElbow_R_CTL_GRP" -p "loShoulder_R_CTL";
	rename -uid "DDF15D26-417A-47EE-ACC8-CD825DD5E0EC";
	setAttr ".t" -type "double3" -9.1427725554122858 0 -1.0824674490095276e-15 ;
	setAttr ".r" -type "double3" 0.097090872588109708 -4.2245823943083787 3.5699958052974927 ;
	setAttr ".s" -type "double3" 0.99999999999999978 0.99999999999999944 0.99999999999999978 ;
createNode transform -n "loElbow_R_CTL" -p "loElbow_R_CTL_GRP";
	rename -uid "F06C358B-496A-988E-5575-1ABDA9A17CBD";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loElbow_R_CTLShape" -p "loElbow_R_CTL";
	rename -uid "33242199-4999-8DE0-9507-27B8D5890885";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		6.8832901557850277e-15 4.9177182419927812 4.9177182419927714
		9.381261778758371e-15 6.9547038337557554 -5.1806507055814956e-15
		8.464948973720331e-15 4.9177182419927705 -4.9177182419927803
		4.6711153544860533e-15 -5.4680150251073073e-15 -6.9547038337557581
		2.2213720181597497e-16 -4.9177182419927803 -4.9177182419927714
		-2.2758344211573715e-15 -6.9547038337557598 5.3539352653158422e-15
		-1.3595216161193283e-15 -4.9177182419927705 4.9177182419927803
		2.4343120031149478e-15 4.8801375937003186e-15 6.9547038337557581
		6.8832901557850277e-15 4.9177182419927812 4.9177182419927714
		9.381261778758371e-15 6.9547038337557554 -5.1806507055814956e-15
		8.464948973720331e-15 4.9177182419927705 -4.9177182419927803
		;
	setAttr ".adot" yes;
createNode transform -n "loWrist_R_CTL_GRP" -p "loElbow_R_CTL";
	rename -uid "7B817544-4FCD-D6D3-5CA0-B79FE7FA58C7";
	setAttr ".t" -type "double3" -8.4152140198571388 3.5527136788005009e-15 -1.1102230246251565e-15 ;
	setAttr ".r" -type "double3" 0.33378093321428737 -0.8897968796219754 -19.248887884355788 ;
	setAttr ".s" -type "double3" 1.0000000000000002 1.0000000000000002 1.0000000000000002 ;
createNode transform -n "loWrist_R_CTL" -p "loWrist_R_CTL_GRP";
	rename -uid "A431B7B2-491C-3CB4-6D67-A59E0E242809";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loWrist_R_CTLShape" -p "loWrist_R_CTL";
	rename -uid "E18210DC-4784-74DA-C7F8-27AF23BD93EB";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		9.5731574025562659e-15 3.8096974388567215 3.8096974388567006
		1.2933319231202742e-14 5.3877257865692059 -9.0507024954419582e-15
		1.079844955052435e-14 3.8096974388567073 -3.809697438856714
		4.4191260655633654e-15 -1.995877538624416e-15 -5.3877257865692005
		-2.4677300449552633e-15 -3.8096974388567069 -3.8096974388567006
		-5.8278918736017446e-15 -5.3877257865691943 8.8409133755272183e-15
		-3.6930221929233479e-15 -3.8096974388566931 3.809697438856714
		2.6863012920376357e-15 1.5751310646775318e-14 5.3877257865692005
		9.5731574025562659e-15 3.8096974388567215 3.8096974388567006
		1.2933319231202742e-14 5.3877257865692059 -9.0507024954419582e-15
		1.079844955052435e-14 3.8096974388567073 -3.809697438856714
		;
	setAttr ".adot" yes;
createNode transform -n "loThumb1_R_CTL_GRP" -p "loWrist_R_CTL";
	rename -uid "CF10003E-4A2F-71A7-8E04-8C944FF4945A";
	setAttr ".t" -type "double3" -0.42272365381576549 -3.6617016049173188 -0.97564700022626516 ;
	setAttr ".r" -type "double3" 90.868851202964152 -12.887106561830631 81.111154518828982 ;
	setAttr ".s" -type "double3" 1.0000000000000002 1.0000000000000002 1.0000000000000002 ;
createNode transform -n "loThumb1_R_CTL" -p "loThumb1_R_CTL_GRP";
	rename -uid "50D95048-4EF8-E920-CBC6-DE88954986A1";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loThumb1_R_CTLShape" -p "loThumb1_R_CTL";
	rename -uid "0C30EEEC-4345-2B2F-D8E6-6D956053FD56";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.3202467469802629e-14 2.2142686752308731 2.2142686752308478
		2.7356660502495238e-14 3.1314487912493854 -1.6506773529148787e-14
		2.9647955813688353e-14 2.2142686752308438 -2.2142686752308691
		1.873414368548693e-14 -2.0088898286583457e-14 -3.1314487912493827
		1.0083872453993777e-15 -2.2142686752308727 -2.2142686752308407
		-1.3145805787293244e-14 -3.1314487912493871 2.3490267456579327e-14
		-1.5437101098486352e-14 -2.2142686752308438 2.2142686752308762
		-4.5232889702849259e-15 1.9824198578876699e-14 3.1314487912493898
		1.3202467469802629e-14 2.2142686752308731 2.2142686752308478
		2.7356660502495238e-14 3.1314487912493854 -1.6506773529148787e-14
		2.9647955813688353e-14 2.2142686752308438 -2.2142686752308691
		;
	setAttr ".adot" yes;
createNode transform -n "loThumb2_R_CTL_GRP" -p "loThumb1_R_CTL";
	rename -uid "84E1872D-49EA-16DF-A229-6EBA54EB1606";
	setAttr ".t" -type "double3" -2.5787619122361605 1.2434497875801753e-14 -1.4210854715202004e-14 ;
	setAttr ".r" -type "double3" 0.92887510270755669 16.780262712054846 -10.148617095669959 ;
	setAttr ".s" -type "double3" 1.0000000000000002 1 0.99999999999999989 ;
createNode transform -n "loThumb2_R_CTL" -p "loThumb2_R_CTL_GRP";
	rename -uid "60B0F6CD-447D-437C-2CF5-A8B4C4CCD822";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loThumb2_R_CTLShape" -p "loThumb2_R_CTL";
	rename -uid "DE30BD9E-4462-0221-EC2E-CEB1C9F166A7";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.7721613485040391e-14 2.1888070040540417 2.1888070040540151
		2.1435975698989315e-14 3.0954405505504252 -1.7693720952079717e-14
		1.2593434071170395e-14 2.1888070040540111 -2.1888070040540382
		-3.6261704386887264e-15 -2.1275507514234951e-14 -3.095440550550423
		-1.7721613485040391e-14 -2.1888070040540417 -2.188807004054008
		-2.1435975698989327e-14 -3.095440550550427 2.4678616980772492e-14
		-1.2593434071170395e-14 -2.1888070040540111 2.1888070040540453
		3.6261704386887241e-15 2.1013851564042187e-14 3.0954405505504301
		1.7721613485040391e-14 2.1888070040540417 2.1888070040540151
		2.1435975698989315e-14 3.0954405505504252 -1.7693720952079717e-14
		1.2593434071170395e-14 2.1888070040540111 -2.1888070040540382
		;
	setAttr ".adot" yes;
createNode transform -n "loFingerA1_R_CTL_GRP" -p "loWrist_R_CTL";
	rename -uid "40BA12CF-4592-580F-A456-529828143D6D";
	setAttr ".t" -type "double3" -5.5213954127458393 -1.7559403927372053 0.086046692341724906 ;
	setAttr ".r" -type "double3" 0.10759895188056177 15.754415943653239 20.956755684058329 ;
	setAttr ".s" -type "double3" 0.99999999999999989 1 0.99999999999999989 ;
createNode transform -n "loFingerA1_R_CTL" -p "loFingerA1_R_CTL_GRP";
	rename -uid "B8398A7F-4106-6130-082A-17BD5565CACD";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loFingerA1_R_CTLShape" -p "loFingerA1_R_CTL";
	rename -uid "74D0A867-4086-374E-C7B2-8885DC678ADD";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		5.9758012922527102e-15 1.6213388330659162 1.6213388330658951
		1.0856310290147999e-14 2.2929193669239725 -1.2492266311377917e-14
		9.3773399574051882e-15 1.6213388330659002 -1.6213388330659142
		2.4052510565975577e-15 -7.1847312664669485e-15 -2.292919366923972
		-5.9758012922527086e-15 -1.6213388330659089 -1.6213388330658987
		-1.0856310290148008e-14 -2.2929193669239671 8.8502701495636101e-15
		-9.3773399574051882e-15 -1.6213388330658931 1.6213388330659106
		-2.4052510565975589e-15 1.4096339364460074e-14 2.2929193669239685
		5.9758012922527102e-15 1.6213388330659162 1.6213388330658951
		1.0856310290147999e-14 2.2929193669239725 -1.2492266311377917e-14
		9.3773399574051882e-15 1.6213388330659002 -1.6213388330659142
		;
	setAttr ".adot" yes;
createNode transform -n "loFingerA2_R_CTL_GRP" -p "loFingerA1_R_CTL";
	rename -uid "D05D0416-4F08-2FB7-1505-538E4977F62A";
	setAttr ".t" -type "double3" -1.8027182253475047 3.5527136788005009e-15 -3.5527136788005009e-15 ;
	setAttr ".r" -type "double3" 0.51982383854028846 6.8774812014232953 3.9514721757772766 ;
	setAttr ".s" -type "double3" 0.99999999999999978 1.0000000000000002 1 ;
createNode transform -n "loFingerA2_R_CTL" -p "loFingerA2_R_CTL_GRP";
	rename -uid "BAA36D0F-442C-2B10-6CE4-088F55BCCEB1";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loFingerA2_R_CTLShape" -p "loFingerA2_R_CTL";
	rename -uid "A8889827-4458-ED19-ECC1-369D2E3B3659";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		2.4969707689749016e-14 1.6419293401550525 1.6419293401550483
		1.6909678443371167e-14 2.3220387413055774 -7.8028302316712742e-16
		7.2687148594861451e-15 1.6419293401550481 -1.6419293401550488
		1.6943626511886672e-15 -8.0209199037831957e-16 -2.3220387413055752
		3.4520017406549926e-15 -1.6419293401550488 -1.6419293401550448
		1.1512030987032837e-14 -2.3220387413055756 4.2425803587378395e-15
		2.1152994570917861e-14 -1.6419293401550445 1.6419293401550523
		2.672734677921534e-14 4.1585249642779677e-15 2.3220387413055787
		2.4969707689749016e-14 1.6419293401550525 1.6419293401550483
		1.6909678443371167e-14 2.3220387413055774 -7.8028302316712742e-16
		7.2687148594861451e-15 1.6419293401550481 -1.6419293401550488
		;
	setAttr ".adot" yes;
createNode transform -n "loFingerB1_R_CTL_GRP" -p "loWrist_R_CTL";
	rename -uid "56AAF41F-4B1B-81D8-6456-9CA85328B79C";
	setAttr ".t" -type "double3" -6.7082514341585764 1.7559403927372337 -0.08604669234172535 ;
	setAttr ".r" -type "double3" -2.8335492771617719 15.298551737981018 9.9358856464712542 ;
	setAttr ".s" -type "double3" 1 0.99999999999999989 1 ;
createNode transform -n "loFingerB1_R_CTL" -p "loFingerB1_R_CTL_GRP";
	rename -uid "DBCC1F58-43B3-411F-E0D6-198EC0744F01";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loFingerB1_R_CTLShape" -p "loFingerB1_R_CTL";
	rename -uid "711F921D-4DB2-1C19-FF29-E5BBBD886E55";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-1.6494475330472725e-14 1.4801363956320894 1.4801363956320963
		-1.2145496072813837e-14 2.093228964864934 5.168242022798033e-15
		-4.8441129170756106e-15 1.4801363956320961 -1.4801363956320892
		1.1326229081929691e-15 5.1485820743879703e-15 -2.0932289648649349
		2.283620615270721e-15 -1.4801363956320892 -1.4801363956320963
		-2.0653586423881639e-15 -2.0932289648649354 -5.2497488922481786e-15
		-9.3667417981263939e-15 -1.4801363956320961 1.4801363956320892
		-1.5343477623394973e-14 -5.3255216100787326e-15 2.0932289648649349
		-1.6494475330472725e-14 1.4801363956320894 1.4801363956320963
		-1.2145496072813837e-14 2.093228964864934 5.168242022798033e-15
		-4.8441129170756106e-15 1.4801363956320961 -1.4801363956320892
		;
	setAttr ".adot" yes;
createNode transform -n "loFingerB2_R_CTL_GRP" -p "loFingerB1_R_CTL";
	rename -uid "8A182701-4427-BD05-E20D-8FAC2793C717";
	setAttr ".t" -type "double3" -1.6306823725054471 -1.0658141036401503e-14 -1.7763568394002505e-15 ;
	setAttr ".r" -type "double3" 1.3643449526118958 -5.6302836786382437 -3.4011530161809667 ;
	setAttr ".s" -type "double3" 1 1.0000000000000002 1 ;
createNode transform -n "loFingerB2_R_CTL" -p "loFingerB2_R_CTL_GRP";
	rename -uid "78CD3C2D-46A5-C0C3-8824-239164FB8AF4";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "loFingerB2_R_CTLShape" -p "loFingerB2_R_CTL";
	rename -uid "9197C4C3-4FE7-9BBB-9C43-BCA11683204D";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-8.4969619351810282e-15 1.5215517609229134 1.5215517609229137
		-6.8161371806284059e-15 2.1517991361498479 7.3064793895222679e-16
		-5.304774688285016e-15 1.5215517609229126 -1.5215517609229123
		-4.8482101085036152e-15 -1.777405297753355e-16 -2.1517991361498479
		-5.7138927800209748e-15 -1.5215517609229132 -1.5215517609229119
		-7.3947175345735978e-15 -2.1517991361498492 9.6192140552177588e-16
		-8.906080026916987e-15 -1.5215517609229126 1.5215517609229141
		-9.3626446066983893e-15 -4.1499114715945728e-18 2.1517991361498496
		-8.4969619351810282e-15 1.5215517609229134 1.5215517609229137
		-6.8161371806284059e-15 2.1517991361498479 7.3064793895222679e-16
		-5.304774688285016e-15 1.5215517609229126 -1.5215517609229123
		;
	setAttr ".adot" yes;
createNode transform -n "leg_L_CTL_GRP" -p "cog_CTL";
	rename -uid "D4AADB71-4B21-07E2-1287-E4B49C44E977";
	setAttr ".t" -type "double3" -2 -1.5 7.8 ;
	setAttr ".r" -type "double3" -179.29284226956358 -5.6667833535969994 172.87498365109818 ;
	setAttr ".s" -type "double3" 1.0000000000000002 1 1 ;
createNode transform -n "leg_L_CTL" -p "leg_L_CTL_GRP";
	rename -uid "B0584A26-4A69-DD31-72DF-76ACE6552A34";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "leg_L_CTLShape" -p "leg_L_CTL";
	rename -uid "02E8C08A-4933-3EB5-26B5-BFB699744D35";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		1.9999999999999998 7.4897828957489709 -7.4897828957489674
		2.000000000000004 10.59215255039822 1.3969476583365759e-15
		2.0000000000000058 7.4897828957489638 7.4897828957489692
		2.000000000000004 -4.0499438421741028e-15 10.592152550398223
		2 -7.4897828957489718 7.4897828957489638
		1.9999999999999962 -10.592152550398227 -4.5372204577051373e-15
		1.9999999999999944 -7.4897828957489656 -7.4897828957489727
		1.999999999999996 1.6002825629019057e-15 -10.592152550398227
		1.9999999999999998 7.4897828957489709 -7.4897828957489674
		2.000000000000004 10.59215255039822 1.3969476583365759e-15
		2.0000000000000058 7.4897828957489638 7.4897828957489692
		;
	setAttr ".adot" yes;
createNode transform -n "knee_L_CTL_GRP" -p "leg_L_CTL";
	rename -uid "D7D32DE0-41AE-320D-9A7B-FCB7BF299F18";
	setAttr ".t" -type "double3" 4.0509258201058147 -1.2212453270876722e-15 -1.7763568394002505e-15 ;
	setAttr ".r" -type "double3" 0 -1.075129710596969 -7.0900169972689158 ;
	setAttr ".s" -type "double3" 0.99999999999999989 1 1 ;
createNode transform -n "knee_L_CTL" -p "knee_L_CTL_GRP";
	rename -uid "358F70DA-4BFA-0F33-9528-74AC8AD01D67";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "knee_L_CTLShape" -p "knee_L_CTL";
	rename -uid "CD9D4168-4874-4B43-A2AF-359EA94665F1";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-6.1422150626133273e-15 7.0892438051825621 -7.0892438051825701
		-5.2158894580146254e-15 10.025704736258628 -6.7179652384375275e-15
		-7.1388367629658106e-16 7.0892438051825692 7.0892438051825613
		4.7265883534924675e-15 6.6238021816491868e-15 10.025704736258632
		7.9185719020135778e-15 -7.0892438051825613 7.0892438051825701
		6.9922462974148806e-15 -10.025704736258636 7.1083495780392149e-15
		2.4902405156968315e-15 -7.0892438051825692 -7.0892438051825613
		-2.9502315140922163e-15 -7.4712696927442977e-15 -10.025704736258632
		-6.1422150626133273e-15 7.0892438051825621 -7.0892438051825701
		-5.2158894580146254e-15 10.025704736258628 -6.7179652384375275e-15
		-7.1388367629658106e-16 7.0892438051825692 7.0892438051825613
		;
	setAttr ".adot" yes;
createNode transform -n "ankle_L_CTL_GRP" -p "knee_L_CTL";
	rename -uid "61D7FA6E-4E9E-3158-7208-4DB376CC7D8A";
	setAttr ".t" -type "double3" 3.7121422386541161 2.0384251526028623e-15 0 ;
	setAttr ".r" -type "double3" -4.6354634269025121 0 90.000000000000185 ;
	setAttr ".s" -type "double3" 1 0.99999999999999989 0.99999999999999989 ;
createNode transform -n "ankle_L_CTL" -p "ankle_L_CTL_GRP";
	rename -uid "E45F70DE-451B-C9CE-A364-7BACC9D6DC4A";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "ankle_L_CTLShape" -p "ankle_L_CTL";
	rename -uid "A13DE2F2-4D4D-A2FA-4B22-7CAE9AA95C3E";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 6;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		5.2803062108606262 -3.1874038904877163e-14 -5.2803062108606662
		-2.6837446392215572e-14 -1.7782000400462112e-14 -7.4674806568820111
		-5.2803062108606653 6.7264927724198344e-15 -5.2803062108606262
		-7.4674806568820138 2.7294697706422848e-14 2.690758219051175e-14
		-5.2803062108606262 3.1874038904877163e-14 5.2803062108606653
		2.6546675061779109e-14 1.7782000400462122e-14 7.4674806568820156
		5.2803062108606653 -6.7264927724198344e-15 5.2803062108606262
		7.4674806568820138 -2.7294697706422845e-14 -2.6276360005845628e-14
		5.2803062108606262 -3.1874038904877163e-14 -5.2803062108606662
		-2.6837446392215572e-14 -1.7782000400462112e-14 -7.4674806568820111
		-5.2803062108606653 6.7264927724198344e-15 -5.2803062108606262
		;
	setAttr ".adot" yes;
createNode transform -n "leg_R_CTL_GRP" -p "cog_CTL";
	rename -uid "CF1FB85D-44F9-D8A6-D3D4-B0AB5895DBA4";
	setAttr ".t" -type "double3" -2 -1.5 -7.8 ;
	setAttr ".r" -type "double3" -179.29284226956307 -5.6667833535970216 -7.1250163489018448 ;
	setAttr ".s" -type "double3" 1 1 1.0000000000000002 ;
createNode transform -n "leg_R_CTL" -p "leg_R_CTL_GRP";
	rename -uid "62B01FF6-4921-DF2F-D83D-16A47AB0A788";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "leg_R_CTLShape" -p "leg_R_CTL";
	rename -uid "32421BDD-46C0-150F-1C23-F3843DF80740";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-1.9999999999999927 7.4897828957489772 7.4897828957489638
		-1.9999999999999889 10.592152550398222 -5.9538126695318196e-15
		-1.9999999999999902 7.4897828957489629 -7.4897828957489727
		-1.9999999999999956 -6.2753405016565218e-15 -10.592152550398223
		-2.0000000000000022 -7.4897828957489727 -7.4897828957489603
		-2.0000000000000058 -10.592152550398225 9.0940854689003865e-15
		-2.0000000000000044 -7.4897828957489594 7.4897828957489763
		-1.9999999999999991 8.4886159258099829e-15 10.592152550398227
		-1.9999999999999927 7.4897828957489772 7.4897828957489638
		-1.9999999999999889 10.592152550398222 -5.9538126695318196e-15
		-1.9999999999999902 7.4897828957489629 -7.4897828957489727
		;
	setAttr ".adot" yes;
createNode transform -n "knee_R_CTL_GRP" -p "leg_R_CTL";
	rename -uid "B2D1E566-4AEA-17F3-5AC1-3C916145C571";
	setAttr ".t" -type "double3" -4.0509258201058191 3.3306690738754696e-15 -1.7763568394002505e-15 ;
	setAttr ".r" -type "double3" 0 -1.0751297105970596 -7.0900169972689566 ;
	setAttr ".s" -type "double3" 0.99999999999999989 1 0.99999999999999967 ;
createNode transform -n "knee_R_CTL" -p "knee_R_CTL_GRP";
	rename -uid "F0B9AB37-4C83-F282-6CF3-4ABA9CBCA2BA";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "knee_R_CTLShape" -p "knee_R_CTL";
	rename -uid "FBEEB039-4ADF-E820-6D0E-3BB6DBD85FEE";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		-4.8342140506519708e-15 7.089243805182563 7.0892438051825746
		-1.1534920181837611e-14 10.025704736258634 1.1260639122860268e-14
		-1.1998909383847142e-14 7.0892438051825755 -7.0892438051825604
		-5.9543830749380426e-15 1.1166476066071922e-14 -10.025704736258634
		3.0578572112517188e-15 -7.0892438051825621 -7.0892438051825746
		9.7585633424373679e-15 -10.025704736258641 -1.1651023462461959e-14
		1.0222552544446892e-14 -7.0892438051825755 7.0892438051825604
		4.178026235537793e-15 -1.2013943577167032e-14 10.025704736258634
		-4.8342140506519708e-15 7.089243805182563 7.0892438051825746
		-1.1534920181837611e-14 10.025704736258634 1.1260639122860268e-14
		-1.1998909383847142e-14 7.0892438051825755 -7.0892438051825604
		;
	setAttr ".adot" yes;
createNode transform -n "ankle_R_CTL_GRP" -p "knee_R_CTL";
	rename -uid "00203927-45AD-5227-4698-1CA4555FBE5E";
	setAttr ".t" -type "double3" -3.7121422386541156 -3.9336763883822462e-15 1.7763568394002505e-15 ;
	setAttr ".r" -type "double3" -4.6354634269021142 3.9756933518293969e-16 90.000000000000185 ;
	setAttr ".s" -type "double3" 0.99999999999999989 1.0000000000000004 1 ;
createNode transform -n "ankle_R_CTL" -p "ankle_R_CTL_GRP";
	rename -uid "001752B2-42D0-26FB-8233-AF9F0AB85B70";
	setAttr -l on -k off ".v";
	setAttr -l on -k off ".tx";
	setAttr -l on -k off ".ty";
	setAttr -l on -k off ".tz";
	setAttr -l on -k off ".sx";
	setAttr -l on -k off ".sy";
	setAttr -l on -k off ".sz";
createNode nurbsCurve -n "ankle_R_CTLShape" -p "ankle_R_CTL";
	rename -uid "D1CE112B-463E-D677-0E7D-17A861480332";
	setAttr -k off ".v";
	setAttr ".ove" yes;
	setAttr ".ovc" 13;
	setAttr ".ls" 2;
	setAttr ".cc" -type "nurbsCurve" 
		3 8 2 no 3
		13 -2 -1 0 1 2 3 4 5 6 7 8 9 10
		11
		5.2803062108606307 3.2869847117224811e-14 5.2803062108606618
		-2.149768189564043e-14 6.8439916796157186e-14 7.4674806568820111
		-5.2803062108606609 6.3918811423586891e-14 5.2803062108606307
		-7.4674806568820138 2.1954933209847706e-14 -2.1567817693936873e-14
		-5.2803062108606307 -3.2869847117224805e-14 -5.2803062108606609
		2.1206910565203964e-14 -6.8439916796157224e-14 -7.4674806568820156
		5.2803062108606609 -6.3918811423586891e-14 -5.2803062108606307
		7.4674806568820138 -2.1954933209847713e-14 2.0936595509270751e-14
		5.2803062108606307 3.2869847117224811e-14 5.2803062108606618
		-2.149768189564043e-14 6.8439916796157186e-14 7.4674806568820111
		-5.2803062108606609 6.3918811423586891e-14 5.2803062108606307
		;
	setAttr ".adot" yes;
createNode transform -n "geometry_GRP" -p "buddy_rig";
	rename -uid "B2795283-4EDE-080E-F1F2-0F870784A5C0";
	setAttr ".it" no;
createNode transform -n "buddy_body" -p "geometry_GRP";
	rename -uid "28AAFFAC-48DB-4067-751B-21881C3E0977";
	setAttr -l on ".tx";
	setAttr -l on ".ty";
	setAttr -l on ".tz";
	setAttr -l on ".rx";
	setAttr -l on ".ry";
	setAttr -l on ".rz";
	setAttr -l on ".sx";
	setAttr -l on ".sy";
	setAttr -l on ".sz";
	setAttr ".it" no;
createNode mesh -n "buddy_bodyShape" -p "buddy_body";
	rename -uid "4BF1B6C7-422A-B8A1-CF18-33A68699A966";
	setAttr -k off ".v";
	setAttr -s 2 ".iog[0].og";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".pv" -type "double2" 0.29035723954439163 0.24122830480337143 ;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
	setAttr ".dr" 3;
	setAttr ".dsm" 2;
	setAttr ".vcs" 2;
createNode mesh -n "buddy_bodyShapeOrig" -p "buddy_body";
	rename -uid "7779B1AA-40EE-D8A1-F975-D387869FC259";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 1961 ".uvst[0].uvsp";
	setAttr ".uvst[0].uvsp[0:249]" -type "float2" 0.66135234 0.066164799 0.68033296
		 0.065691002 0.68237334 0.08492893 0.66379386 0.085875399 0.69972134 0.083157934 0.69795531
		 0.06486351 0.71576369 0.062602282 0.71727222 0.080834717 0.73332059 0.06022916 0.73498702
		 0.078575701 0.52253687 0.099323288 0.75000691 0.05734574 0.54221362 0.073093966 0.54538774
		 0.09575142 0.56463212 0.071061686 0.56740588 0.092651621 0.58770001 0.090395339 0.58612514
		 0.069289342 0.60557669 0.067952164 0.60705262 0.088479653 0.6246655 0.066864856 0.62635541
		 0.0874338 0.64491343 0.086722352 0.64305168 0.066489205 0.65929663 0.046317525 0.6780929
		 0.046628959 0.69676399 0.046270669 0.7145288 0.044244021 0.73145854 0.041894644 0.74744964
		 0.0391801 0.54130375 0.050682619 0.56346464 0.048722818 0.5842585 0.047632411 0.60354453
		 0.046675153 0.62258732 0.046418913 0.64059716 0.046216257 0.65591931 0.026905183
		 0.67611945 0.027873907 0.69592702 0.027780619 0.71453148 0.025745954 0.73101777 0.024065252
		 0.74640346 0.022350188 0.54225433 0.02859075 0.5640558 0.027758997 0.58343625 0.026418816
		 0.60118723 0.025879029 0.61913985 0.025987957 0.6373015 0.026256029 0.7249819 0.44254902
		 0.70424175 0.43131953 0.6947763 0.40898925 0.70037794 0.3880578 0.71219546 0.37383503
		 0.73030448 0.36737999 0.75510097 0.3704789 0.7710346 0.38474834 0.77613509 0.40841854
		 0.77381885 0.42604446 0.76310432 0.44135335 0.7443971 0.44676724 0.68039107 0.15473749
		 0.67909139 0.14934753 0.6854859 0.14578581 0.68847793 0.15556504 0.67488289 0.14484127
		 0.66880339 0.14366408 0.66829991 0.13588233 0.67886376 0.13796408 0.66320872 0.14633627
		 0.65853274 0.14042534 0.65350693 0.14991708 0.65996128 0.15211125 0.65939653 0.15866031
		 0.65199155 0.16042627 0.67912316 0.15902013 0.68508697 0.1611038 0.66448087 0.15342462
		 0.66782182 0.1538038 0.66793507 0.15647878 0.67255765 0.1608599 0.67309487 0.15437527
		 0.67244238 0.15251906 0.67502737 0.1514072 0.6757881 0.15460564 0.67034006 0.1534244
		 0.67125839 0.15595348 0.67273271 0.14872515 0.6717788 0.15073903 0.6699304 0.15091594
		 0.6694715 0.14797457 0.66784042 0.15139264 0.66617501 0.14961296 0.67465264 0.15769942
		 0.7032572 0.17996696 0.7008068 0.17636086 0.70588392 0.17466393 0.70624155 0.17897445
		 0.64825106 0.19298656 0.70084697 0.18271995 0.64005625 0.20063744 0.63890332 0.1934499
		 0.63431269 0.20239581 0.63090295 0.19619448 0.62740999 0.21109433 0.71456873 0.17762315
		 0.62460661 0.20127003 0.62992978 0.20582654 0.70978004 0.17921078 0.71084481 0.1753782
		 0.70675552 0.18160915 0.7047646 0.18178743 0.63680863 0.20647378 0.64009857 0.20475297
		 0.70716238 0.18408233 0.6434797 0.20696627 0.70886964 0.1813156 0.63360941 0.21229111
		 0.63341147 0.20822217 0.69772118 0.17068739 0.70498997 0.16897887 0.71295154 0.16981235
		 0.71618843 0.17454106 0.6162771 0.19533403 0.62595004 0.18805596 0.6367383 0.18396245
		 0.64766628 0.18232723 0.73859531 0.16495931 0.73780954 0.17149664 0.73162723 0.17178577
		 0.72921187 0.16563661 0.72663689 0.17397708 0.72318411 0.17748031 0.60963708 0.2068053
		 0.72168767 0.16894275 0.60040069 0.21053042 0.60399318 0.19980234 0.5922243 0.21005274
		 0.58410603 0.21256891 0.57976735 0.20243338 0.59145111 0.19944128 0.57596326 0.21803258
		 0.74743414 0.16702279 0.73381913 0.17973796 0.73275656 0.17636365 0.73707938 0.17657833
		 0.73624629 0.17933165 0.59037578 0.22358914 0.59380853 0.22319834 0.5948174 0.22797807
		 0.73812652 0.18143693 0.58776742 0.21978937 0.59315962 0.21789254 0.74138749 0.17846113
		 0.72952354 0.17789945 0.73126757 0.18001583 0.73070943 0.18291181 0.60429871 0.22091372
		 0.59737802 0.22233115 0.59848547 0.2179846 0.69194853 0.16383173 0.64681524 0.16936658
		 0.63399702 0.17245717 0.68623012 0.12480638 0.69523144 0.13958432 0.70220959 0.16094488
		 0.71199769 0.16013056 0.6997124 0.15170299 0.71301132 0.15035224 0.7088986 0.13518228
		 0.7043159 0.11914153 0.71852964 0.15907592 0.72805786 0.15660685 0.73936385 0.15620825
		 0.74998999 0.15788987 0.57488579 0.18994789 0.58807862 0.18594091 0.60123736 0.18389745
		 0.61022782 0.18113042 0.62211943 0.17608009 0.64351612 0.15899594 0.63141948 0.15988193
		 0.64395386 0.14585787 0.64910036 0.12869346 0.61723405 0.16289176 0.60221308 0.16946001
		 0.62981397 0.14484663 0.62976444 0.12724335 0.61392134 0.14641328 0.61148429 0.12826437
		 0.59738445 0.14985909 0.5849129 0.17086519 0.57991076 0.15311573 0.56837237 0.17529835
		 0.56103539 0.15819 0.75300729 0.14583209 0.75501555 0.13056952 0.73908305 0.14463085
		 0.73803461 0.13058117 0.72525054 0.14595528 0.72278422 0.13198727 0.72044116 0.11619431
		 0.73756021 0.11416674 0.60910815 0.10877444 0.59020609 0.11077292 0.59350139 0.13072301
		 0.57058203 0.11369474 0.57445955 0.13400869 0.66740417 0.1231329 0.64666969 0.10718531
		 0.66584885 0.10522319 0.62806475 0.10758258 0.68442386 0.10434587 0.71882427 0.098751962
		 0.70178038 0.10140496 0.73647392 0.096740425 0.55465657 0.13854213 0.54909503 0.11760922
		 0.75503701 0.11345267 0.52727526 0.12255295 0.31966704 0.065691002 0.31762666 0.08492893
		 0.30027866 0.083157934 0.30204469 0.06486351 0.28272778 0.080834717 0.28423631 0.062602282
		 0.26667941 0.06022916 0.26501295 0.078575701 0.24767263 0.07637208 0.47949994 0.075828396
		 0.45461226 0.09575142 0.45778638 0.073093966 0.43536788 0.071061686 0.43259412 0.092651621
		 0.41229999 0.090395339 0.41387486 0.069289342 0.39294738 0.088479653 0.39442331 0.067952164
		 0.3753345 0.066864856 0.37364459 0.0874338 0.35508657 0.086722352 0.35694832 0.066489205
		 0.33620614 0.085875399 0.33864766 0.066164799 0.2854712 0.044244021 0.30323601 0.046270669
		 0.26854146 0.041894644 0.45869625 0.050682619 0.48030043 0.052439824 0.43653536 0.048722818
		 0.39645547 0.046675153;
	setAttr ".uvst[0].uvsp[250:499]" 0.4157415 0.047632411 0.37741268 0.046418913
		 0.34070337 0.046317525 0.35940284 0.046216257 0.3219071 0.046628959 0.30407298 0.027780619
		 0.28546852 0.025745954 0.29962209 0.3880578 0.3052237 0.40898931 0.26898223 0.024065252
		 0.28780463 0.373835 0.25359654 0.022350188 0.45774567 0.02859075 0.24489918 0.37047884
		 0.26969564 0.36737993 0.4359442 0.027758997 0.22896564 0.38474834 0.41656375 0.026418816
		 0.39881277 0.025879029 0.22618127 0.42604446 0.22386491 0.40841854 0.38086015 0.025987957
		 0.2368958 0.44135335 0.3626985 0.026256029 0.34408069 0.026905183 0.27501822 0.44254911
		 0.25560302 0.4467673 0.32388055 0.027873907 0.29575825 0.43131965 0.32726729 0.14872515
		 0.32497263 0.1514072 0.32090861 0.14934753 0.32511711 0.14484127 0.33119661 0.14366408
		 0.3305285 0.14797457 0.33679128 0.14633627 0.33382499 0.14961296 0.34003872 0.15211125
		 0.33551913 0.15342462 0.34060347 0.15866031 0.32087684 0.15902013 0.32534736 0.15769942
		 0.33534759 0.15799524 0.31960893 0.15473749 0.3242119 0.15460564 0.33217818 0.1538038
		 0.33215958 0.15139264 0.3282212 0.15073903 0.32755762 0.15251906 0.3300696 0.15091594
		 0.32965994 0.1534244 0.32690513 0.15437527 0.32874161 0.15595348 0.33206493 0.15647878
		 0.3145141 0.14578581 0.31152207 0.15556504 0.32113624 0.13796408 0.33170009 0.13588233
		 0.34146726 0.14042534 0.34649307 0.14991708 0.34800845 0.16042627 0.31491303 0.1611038
		 0.29501003 0.16897887 0.29411608 0.17466393 0.28915519 0.1753782 0.28704846 0.16981235
		 0.35174894 0.19298656 0.2991932 0.17636086 0.30227882 0.17068739 0.30824977 0.17537279
		 0.3632617 0.18396245 0.36109668 0.1934499 0.3837229 0.19533403 0.37539339 0.20127003
		 0.36909705 0.19619448 0.37404996 0.18805596 0.38443714 0.2046489 0.28543127 0.17762315
		 0.29375845 0.17897445 0.29324448 0.18160915 0.29113036 0.1813156 0.29021996 0.17921078
		 0.36319137 0.20647378 0.36568731 0.20239581 0.37007022 0.20582654 0.36658853 0.20822217
		 0.36639059 0.21229111 0.29283762 0.18408233 0.3565203 0.20696627 0.2952354 0.18178743
		 0.2967428 0.17996696 0.29915303 0.18271995 0.35994375 0.20063744 0.35990143 0.20475297
		 0.37259001 0.21109433 0.26724344 0.17636365 0.26837277 0.17178577 0.27336311 0.17397708
		 0.27047646 0.17789945 0.27681589 0.17748031 0.39570129 0.22091372 0.39959931 0.21053042
		 0.40151453 0.2179846 0.4077757 0.21005274 0.40684038 0.21789254 0.41589397 0.21256891
		 0.42403674 0.21803258 0.25861251 0.17846113 0.41223258 0.21978937 0.26219046 0.17149664
		 0.26292062 0.17657833 0.26618087 0.17973796 0.26873243 0.18001583 0.26929057 0.18291181
		 0.4051826 0.22797807 0.40619147 0.22319834 0.40262198 0.22233115 0.26375371 0.17933165
		 0.26187348 0.18143693 0.40962422 0.22358914 0.26140469 0.16495931 0.27078813 0.16563661
		 0.27831233 0.16894275 0.27981645 0.1736744 0.39600682 0.19980234 0.40854889 0.19944128
		 0.42023265 0.20243338 0.43093646 0.20841214 0.30805147 0.16383173 0.35318476 0.16936658
		 0.36600298 0.17245717 0.31376988 0.12480638 0.30476856 0.13958432 0.29779041 0.16094488
		 0.28800231 0.16013056 0.28698868 0.15035224 0.3002876 0.15170299 0.2911014 0.13518228
		 0.2956841 0.11914153 0.28147036 0.15907592 0.27194214 0.15660685 0.26063615 0.15620825
		 0.4396584 0.19731751 0.42511421 0.18994789 0.41192138 0.18594091 0.39876264 0.18389745
		 0.37788057 0.17608009 0.38977218 0.18113042 0.35648388 0.15899594 0.36858052 0.15988193
		 0.35604614 0.14585787 0.35089964 0.12869346 0.38276595 0.16289176 0.39778692 0.16946001
		 0.37018603 0.14484663 0.37023556 0.12724335 0.38607866 0.14641328 0.38851571 0.12826437
		 0.40261555 0.14985909 0.4150871 0.17086519 0.42008924 0.15311573 0.43162763 0.17529835
		 0.43896461 0.15819 0.24699272 0.14583209 0.45791131 0.16531055 0.26091695 0.14463085
		 0.26196539 0.13058117 0.27474946 0.14595528 0.27721578 0.13198727 0.27955884 0.11619431
		 0.26243979 0.11416674 0.39089185 0.10877444 0.40979391 0.11077292 0.40649861 0.13072301
		 0.42941797 0.11369474 0.42554045 0.13400869 0.33259583 0.1231329 0.33415115 0.10522319
		 0.35333031 0.10718531 0.37193525 0.10758258 0.31557614 0.10434587 0.28117573 0.098751962
		 0.29821962 0.10140496 0.26352608 0.096740425 0.44534343 0.13854213 0.45090497 0.11760922
		 0.46622288 0.14476763 0.47272474 0.12255295 0.48858708 0.51881635 0.49015182 0.50984859
		 0.50003016 0.51080936 0.50002152 0.52037126 0.51141304 0.51881427 0.50984824 0.50985891
		 0.47893339 0.50655955 0.47386342 0.51486886 0.52106673 0.50655532 0.5261367 0.51487023
		 0.46799594 0.49661726 0.45854571 0.49829373 0.53200406 0.49661547 0.54145432 0.49829462
		 0.45753375 0.47864452 0.46422744 0.48051634 0.54246628 0.47864497 0.53577268 0.48051533
		 0.46018958 0.46229145 0.46740818 0.46701685 0.53981054 0.46229127 0.53259194 0.46701702
		 0.4689123 0.44838297 0.47622269 0.45646894 0.5310877 0.44838297 0.52377731 0.45646885
		 0.48725331 0.44978914 0.4837372 0.44033322 0.51274693 0.4497892 0.51626289 0.44033319
		 0.50002944 0.44734934 0.50002086 0.43849927 0.55109054 0.50758302 0.54042327 0.51993001
		 0.45957679 0.51993042 0.44890958 0.50758332 0.56601942 0.49956715 0.56045604 0.48871663
		 0.4339807 0.49956721 0.43954393 0.48871681 0.55920696 0.47264448 0.44079316 0.4726446
		 0.5548104 0.45451167 0.44518965 0.45451173 0.50002605 0.421074 0.52181071 0.42317823
		 0.47818941 0.42317823 0.54671371 0.43133622 0.45328641 0.43133625 0.55106527 0.40204272
		 0.57709372 0.40224838 0.57464278 0.42773539 0.4229064 0.40224844 0.44893479 0.40204275
		 0.42535746 0.42773545 0.50001758 0.39789164 0.52509224 0.39907891 0.47490767 0.39907894
		 0.42122447 0.48600131 0.4222565 0.46965364 0.57774353 0.46965352 0.57877558 0.48600122;
	setAttr ".uvst[0].uvsp[500:749]" 0.57556844 0.45084563 0.42443168 0.45084569
		 0.58085525 0.49733698 0.41914475 0.49733716 0.53472209 0.53398079 0.52066517 0.53176361
		 0.46527803 0.53398085 0.47933507 0.53176373 0.51108718 0.53266436 0.48891306 0.53266412
		 0.50002813 0.53279763 0.50002164 0.54783267 0.48883504 0.54766798 0.51116508 0.54766792
		 0.47853023 0.5471229 0.52146971 0.54712284 0.46662292 0.54772782 0.53337717 0.54772776
		 0.50002313 0.56420547 0.48729062 0.56399626 0.5127095 0.5639962 0.47621173 0.56319785
		 0.52378845 0.56319785 0.46492231 0.56029874 0.53507781 0.56029868 0.5000205 0.58193713
		 0.4835667 0.58054048 0.5164333 0.58054042 0.47084296 0.57831848 0.52915716 0.57831842
		 0.46017709 0.57049084 0.53982306 0.57049078 0.45229709 0.58033162 0.4633019 0.58946204
		 0.54770297 0.5803315 0.53669816 0.58946192 0.44092637 0.58857071 0.44715506 0.60072809
		 0.55907363 0.58857059 0.552845 0.60072792 0.42474455 0.60559547 0.42586005 0.5933997
		 0.57525539 0.60559529 0.57414007 0.59339947 0.38898224 0.58227426 0.40644634 0.5914765
		 0.40177965 0.60085821 0.38126612 0.58932239 0.59355366 0.5914762 0.59822047 0.60085797
		 0.36754823 0.57529444 0.37629008 0.56920868 0.63245177 0.57529414 0.618734 0.58932215
		 0.6110177 0.58227396 0.62370992 0.5692085 0.36929709 0.55094576 0.35946858 0.55347306
		 0.63070297 0.55094552 0.64053142 0.55347294 0.37300718 0.52653444 0.36043644 0.5263629
		 0.62699294 0.52653438 0.63956368 0.52636266 0.3852194 0.50942397 0.36956 0.49885756
		 0.61478072 0.50942379 0.63044 0.49885747 0.39990968 0.50081205 0.39871663 0.48818019
		 0.60009032 0.50081193 0.60128349 0.48818004 0.50001687 0.60096407 0.47969076 0.6024726
		 0.52030921 0.60247248 0.46484631 0.67528057 0.50000596 0.67700893 0.50000644 0.72190732
		 0.45544356 0.71945399 0.53515387 0.67528027 0.54455644 0.71945316 0.99242282 0.86696005
		 0.0880979 0.84758061 0.55769873 0.7714588 0.99242294 0.77754903 0.068277061 0.76974708
		 0.931723 0.7697469 0.99242294 0.71220011 0.055605054 0.7082929 0.94439495 0.70829278
		 0.99242294 0.66019076 0.048342168 0.65913302 0.95165789 0.65913296 0.99242294 0.61631966
		 0.044503152 0.61691773 0.95549691 0.61691761 0.45375305 0.61496538 0.54624677 0.6149652
		 0.42193341 0.62680519 0.57806659 0.62680483 0.38760048 0.61863387 0.36464268 0.60432112
		 0.61239964 0.61863351 0.36612952 0.64175969 0.33997709 0.62195045 0.6015501 0.6569944
		 0.6338706 0.64175928 0.34726822 0.58412397 0.63535738 0.60432076 0.65273178 0.58412373
		 0.33678079 0.55847341 0.66321945 0.55847311 0.31999069 0.59670991 0.30486977 0.56644124
		 0.68000925 0.59670967 0.69513035 0.566441 0.33542538 0.52973831 0.66457474 0.52973813
		 0.30858999 0.53367794 0.69141018 0.5336777 0.33613467 0.50121039 0.66386545 0.50121021
		 0.30526793 0.50439566 0.69473195 0.50439554 0.33481032 0.47145724 0.36870825 0.4711543
		 0.63129175 0.47115415 0.66518974 0.47145715 0.2924878 0.47048736 0.70751232 0.47048727
		 0.6020149 0.46904293 0.39798522 0.46904302 0.34140706 0.44351909 0.37133783 0.44597298
		 0.62866235 0.44597289 0.65859306 0.443519 0.31222254 0.43950868 0.6877777 0.43950859
		 0.60046148 0.4481262 0.3995387 0.44812629 0.34695697 0.41729292 0.3736003 0.42192891
		 0.62639976 0.42192888 0.65304327 0.41729286 0.32070896 0.41301993 0.67929125 0.4130199
		 0.60051215 0.42491418 0.39948791 0.42491427 0.35161281 0.39313275 0.37598741 0.39990601
		 0.62401271 0.39990592 0.64838719 0.39313272 0.31871617 0.38615713 0.68128383 0.3861571
		 0.60159022 0.40182754 0.39840978 0.4018276 0.66002291 0.62195009 0.62774879 0.69228804
		 0.5880723 0.70927101 0.56970268 0.6692543 0.41192782 0.70927185 0.37225121 0.6922887
		 0.39844996 0.65699488 0.43029743 0.66925478 0.66042531 0.73536849 0.84654832 0.81264216
		 0.15345189 0.8126424 0.20213142 0.76836145 0.8381685 0.71933937 0.87951314 0.74856681
		 0.12048689 0.74856693 0.16183159 0.71933961 0.86533165 0.68018371 0.90066195 0.6974079
		 0.099338084 0.69740808 0.13466835 0.68018377 0.7638011 0.71938229 0.80909836 0.68446732
		 0.23619902 0.71938246 0.19090173 0.68446743 0.20611167 0.64638931 0.25720274 0.66417062
		 0.8411665 0.65825307 0.15883353 0.65825325 0.17033088 0.62902892 0.78987634 0.61111003
		 0.79388857 0.64638913 0.74279737 0.66417038 0.75116819 0.62130302 0.21012384 0.61111021
		 0.24883193 0.6213032 0.82340437 0.59526849 0.82966936 0.62902874 0.17659575 0.59526861
		 0.91356659 0.65478951 0.086433411 0.65478957 0.88165605 0.64745271 0.11834389 0.64745283
		 0.86069566 0.64100343 0.14117756 0.61818194 0.13930443 0.64100349 0.8566891 0.58687001
		 0.85882258 0.61818177 0.14331087 0.58687007 0.91989791 0.61670035 0.080102116 0.61670047
		 0.88727009 0.6160996 0.11272997 0.61609966 0.88906038 0.58283156 0.11093956 0.58283162
		 0.99242282 0.57867014 0.042675138 0.5793336 0.95732498 0.57933354 0.92265439 0.5808351
		 0.077345699 0.5808351 0.63251364 0.9179334 0.61416376 0.92027646 0.611269 0.89564306
		 0.63059962 0.89134979 0.38583621 0.92027646 0.38873097 0.89564306 0.4065446 0.90133911
		 0.40348139 0.92352867 0.59345543 0.90133911 0.59651864 0.92352867 0.57940686 0.92623234
		 0.57618356 0.90589619 0.42381641 0.90589619 0.42059317 0.92623234 0.56255984 0.9282791
		 0.55915332 0.90939593 0.43744013 0.9282791 0.44084665 0.90939593 0.457773 0.91203284
		 0.22364891 0.92720574 0.77868557 0.90326661 0.54607189 0.92986202 0.75712872 0.92482328
		 0.7580018 0.90114725 0.24199814 0.90114725 0.24287122 0.92482328 0.73607075 0.92282653
		 0.73607469 0.89910591 0.26392925 0.92282653 0.26392531 0.89910591 0.28602034 0.89711761
		 0.28604376 0.92112112 0.71397966 0.89711761 0.71395624 0.92112112 0.69214636 0.91956961;
	setAttr ".uvst[0].uvsp[750:999]" 0.6924665 0.89515549 0.30753353 0.89515549
		 0.30785364 0.91956961 0.6715467 0.91817355 0.67120087 0.8926577 0.32845333 0.91817355
		 0.32879913 0.8926577 0.34978506 0.89164376 0.34872082 0.91766 0.65021491 0.89164376
		 0.65127921 0.91766 0.36940041 0.89134979 0.36748639 0.9179334 0.63357127 0.9438473
		 0.61555898 0.94534159 0.59825361 0.94694763 0.58116829 0.94809145 0.41883174 0.94809145
		 0.40174642 0.94694763 0.56491184 0.94862467 0.43508819 0.94862467 0.54900336 0.94862235
		 0.75626266 0.9477126 0.24373731 0.9477126 0.22603363 0.94983178 0.73570973 0.94633949
		 0.26429027 0.94633949 0.71392089 0.9451645 0.6921097 0.94423324 0.30789027 0.94423324
		 0.28607911 0.9451645 0.67151833 0.94333869 0.3284817 0.94333869 0.65183032 0.94328958
		 0.3664287 0.9438473 0.34816971 0.94328958 0.38444105 0.94534159 0.63487053 0.96937656
		 0.61668825 0.97031277 0.59933782 0.97089028 0.58159649 0.97102487 0.40066221 0.97089034
		 0.41840348 0.97102487 0.28351212 0.51175755 0.28902692 0.53537828 0.56537366 0.97075009
		 0.43462637 0.97075003 0.27257115 0.49646568 0.77366757 0.97123289 0.75648105 0.97012889
		 0.22633246 0.97123289 0.24351892 0.97012889 0.23632348 0.49165994 0.25640345 0.48958698
		 0.73587316 0.96939814 0.26412684 0.96939814 0.21718091 0.50430828 0.71381408 0.96889961
		 0.69230902 0.96848571 0.28618592 0.96889955 0.30769101 0.96848571 0.2049118 0.54981118
		 0.20520669 0.52649295 0.67158031 0.96823633 0.32841966 0.96823633 0.21932617 0.56731796
		 0.65220892 0.96882582 0.34779111 0.96882582 0.3651295 0.96937662 0.26149833 0.57448786
		 0.23954478 0.57487488 0.38331172 0.97031283 0.28392935 0.55701238 0.73850179 0.57448769
		 0.71607077 0.55701208 0.71097314 0.53537804 0.71648788 0.51175737 0.72742891 0.49646559
		 0.74359655 0.48958695 0.76367664 0.49165991 0.78281921 0.50430822 0.79479349 0.52649289
		 0.79508817 0.54981112 0.78067398 0.56731784 0.76045537 0.5748747 0.50001132 0.50780666
		 0.49119958 0.50745499 0.49190146 0.50351185 0.50003064 0.50485563 0.50880045 0.50744265
		 0.50809866 0.50351673 0.48186168 0.50396037 0.4835628 0.49991283 0.51813841 0.5039649
		 0.51643741 0.49990997 0.47332716 0.49449328 0.47751409 0.49035379 0.52667284 0.49449515
		 0.52248597 0.4903532 0.46999443 0.48192543 0.47808701 0.48176819 0.53000557 0.48192605
		 0.52191311 0.48176807 0.47305208 0.46919686 0.47925922 0.47201052 0.52694792 0.46919665
		 0.52074093 0.47201055 0.48026365 0.46075639 0.48439401 0.46560037 0.51973653 0.46075645
		 0.51560622 0.46560031 0.49039632 0.45503467 0.49365157 0.46031246 0.50960368 0.45503461
		 0.50634843 0.46031249 0.50001872 0.45290691 0.50002861 0.45950967 0.59292471 0.80717933
		 0.59843659 0.81203794 0.59288013 0.82201517 0.5839026 0.81512296 0.39403829 0.80846047
		 0.39871749 0.80648899 0.40156338 0.81203794 0.39447406 0.81485701 0.60552597 0.81485701
		 0.61408532 0.81485051 0.61700034 0.8263377 0.60215151 0.82721102 0.38591471 0.81485051
		 0.38851437 0.8077473 0.62344384 0.80860478 0.63146114 0.81912661 0.37655613 0.80860478
		 0.38340887 0.80313957 0.6411072 0.80429488 0.62896454 0.79856402 0.58465707 0.79271489
		 0.64702797 0.78636301 0.37103549 0.79856402 0.38128582 0.7961244 0.58843589 0.80106276
		 0.57900953 0.80637759 0.4153429 0.79271489 0.41156408 0.80106276 0.40484878 0.79746121
		 0.38214621 0.78661287 0.40707526 0.80717933 0.40227887 0.80308402 0.61871421 0.7961244
		 0.61172986 0.79663742 0.60937262 0.79108375 0.61785376 0.78661287 0.38827017 0.79663742
		 0.38726112 0.80080557 0.60078406 0.80036902 0.60361004 0.80281234 0.60128248 0.80648899
		 0.5977211 0.80308402 0.39375868 0.80472302 0.39638999 0.80281234 0.606426 0.79912239
		 0.6017226 0.79536062 0.39068505 0.80281031 0.39357397 0.79912239 0.60596168 0.80846047
		 0.60624135 0.80472302 0.60931492 0.80281031 0.6114856 0.8077473 0.61273885 0.80080557
		 0.6165911 0.80313957 0.59515119 0.79746121 0.39921591 0.80036902 0.39827743 0.79536062
		 0.40147552 0.7900275 0.40711984 0.82201517 0.41609743 0.81512296 0.39784846 0.82721102
		 0.38299969 0.8263377 0.36853883 0.81912661 0.35889277 0.80429488 0.35297206 0.78636301
		 0.4209905 0.80637759 0.54666078 0.80276787 0.55262971 0.80481267 0.54913902 0.81006944
		 0.54481876 0.80720443 0.44411185 0.81454009 0.45086101 0.81006944 0.4546524 0.81352258
		 0.45079687 0.81997997 0.5538944 0.7991969 0.54702771 0.7983585 0.44610563 0.7991969
		 0.44737026 0.80481267 0.43957606 0.80757737 0.43780687 0.79958773 0.68962884 0.73640287
		 0.68696666 0.74607182 0.3157036 0.76065534 0.31303334 0.74607182 0.69787472 0.73762
		 0.69770002 0.74759388 0.28905106 0.75983942 0.29170948 0.7463401 0.30230001 0.74759388
		 0.30311483 0.76014197 0.71398306 0.73244059 0.71724164 0.74186754 0.70829052 0.7463401
		 0.70616442 0.73661166 0.27849635 0.75076175 0.28275836 0.74186754 0.54237056 0.80963504
		 0.54534757 0.81352258 0.54157996 0.80520993 0.54379725 0.80224717 0.45518121 0.80720443
		 0.45842007 0.80520993 0.45940998 0.80684572 0.45762947 0.80963504 0.69804627 0.73019809
		 0.69215244 0.73069882 0.30195373 0.73019809 0.30212528 0.73762 0.29383558 0.73661166
		 0.29618865 0.73041844 0.69841605 0.7210983 0.54236484 0.79951787 0.2925204 0.72411728
		 0.30158395 0.7210983 0.31162995 0.72521019 0.45620278 0.80224717 0.45333919 0.80276787
		 0.45297232 0.7983585 0.31037119 0.73640287 0.30784756 0.73069882 0.54059005 0.80684572
		 0.5372591 0.80717587 0.28601694 0.73244059 0.70381135 0.73041844 0.56042397 0.80757737
		 0.55588818 0.81454009 0.54920316 0.81997997 0.54303432 0.81910253 0.71094894 0.75983942
		 0.69688517 0.76014197 0.68429637 0.76065534 0.66789389 0.75921679 0.53676641 0.83722132;
	setAttr ".uvst[0].uvsp[1000:1249]" 0.53341258 0.83364099 0.53485906 0.83075464
		 0.53985953 0.83384842 0.46880355 0.82987559 0.46514097 0.83075464 0.46471283 0.82684773
		 0.46905002 0.82716852 0.53528714 0.82684773 0.73780847 0.74347198 0.72845757 0.75245786
		 0.54336572 0.82835698 0.26219153 0.74347198 0.47069111 0.82396132 0.74275339 0.75251192
		 0.73232067 0.76365131 0.25724658 0.75251192 0.25110564 0.74422246 0.75178474 0.75736767
		 0.76195717 0.75969392 0.76041174 0.7719906 0.74764311 0.76886499 0.24821526 0.75736767
		 0.24486586 0.74785638 0.53112423 0.83567387 0.53253675 0.83851892 0.23804286 0.75969392
		 0.4688758 0.83567387 0.22959128 0.74818838 0.23759016 0.74940062 0.46658745 0.83364099
		 0.46921363 0.83208632 0.52882838 0.82984352 0.53119648 0.82987559 0.5307864 0.83208632
		 0.52909124 0.83138418 0.47117165 0.82984352 0.47117427 0.82812387 0.76147509 0.74323988
		 0.75735223 0.74157947 0.52664864 0.83011252 0.76568401 0.7397759 0.47326127 0.82730645
		 0.47335139 0.83011252 0.76240981 0.74940062 0.75513411 0.74785638 0.24264774 0.74157947
		 0.24684364 0.74010259 0.77040875 0.74818838 0.47090873 0.83138418 0.47237334 0.83224493
		 0.23852497 0.74323988 0.53094995 0.82716852 0.52882576 0.82812387 0.75250065 0.73478305
		 0.52930892 0.82396132 0.7531563 0.74010259 0.74889433 0.74422246 0.46323362 0.83722132
		 0.46014044 0.83384842 0.45663431 0.82835698 0.45922688 0.82174426 0.2676793 0.76365131
		 0.25235695 0.76886499 0.23958829 0.7719906 0.22642189 0.77496374 0.57174194 0.81015605
		 0.65753663 0.77499038 0.42825809 0.81015605 0.3424634 0.77499038 0.67877001 0.77885133
		 0.32122999 0.77885133 0.59893298 0.84716582 0.58440208 0.83517593 0.40106699 0.84716582
		 0.41559789 0.83517593 0.56560636 0.81967968 0.43439367 0.81967968 0.55819154 0.82721484
		 0.44180843 0.82721484 0.57418239 0.82618713 0.56223071 0.83666003 0.43776926 0.83666003
		 0.42581764 0.82618713 0.57193518 0.8474803 0.42806479 0.8474803 0.58151674 0.86161232
		 0.41848329 0.86161232 0.55218387 0.83406359 0.44781616 0.83406359 0.54606581 0.83982289
		 0.45393416 0.83982289 0.53951657 0.84394944 0.46048346 0.84394944 0.53255486 0.84508598
		 0.22405925 0.79450154 0.75809908 0.79121077 0.24190092 0.79121077 0.74170077 0.78943294
		 0.25829923 0.78943294 0.72648692 0.78693044 0.27351311 0.78693044 0.71050674 0.78340119
		 0.69601905 0.7809245 0.30398095 0.7809245 0.28949326 0.78340119 0.65926898 0.79340893
		 0.34073099 0.79340893 0.67690963 0.79809731 0.32309037 0.79809731 0.65453136 0.81331009
		 0.34546867 0.81331009 0.64506543 0.83712095 0.3549346 0.83712095 0.69650501 0.80103558
		 0.30349499 0.80103558 0.7175796 0.80121344 0.28242043 0.80121344 0.67419696 0.81960231
		 0.32580301 0.81960231 0.67100769 0.84296775 0.32899231 0.84296775 0.69519454 0.82326716
		 0.30480546 0.82326716 0.69373322 0.84638518 0.30626678 0.84638518 0.7160691 0.82578802
		 0.28393093 0.82578802 0.73772562 0.80872953 0.26227432 0.80872953 0.73702967 0.82942778
		 0.26297033 0.82942778 0.75714433 0.8123427 0.24285567 0.8123427 0.75842762 0.83174515
		 0.24157235 0.83174515 0.53193092 0.85380715 0.46806905 0.85380715 0.53252161 0.86501545
		 0.22058168 0.83276635 0.54224205 0.85236895 0.45775798 0.85236895 0.54626441 0.86249906
		 0.45373556 0.86249906 0.55225539 0.84672266 0.44774458 0.84672266 0.55940723 0.85653794
		 0.44059274 0.85653794 0.56614411 0.87018085 0.43385586 0.87018085 0.55075669 0.87585139
		 0.44924328 0.87585139 0.69279039 0.87080508 0.71462017 0.87317258 0.30720958 0.87080508
		 0.28537983 0.87317258 0.28490201 0.84919274 0.71509802 0.84919274 0.73616958 0.87530726
		 0.26383036 0.87530726 0.2632294 0.85168332 0.73677063 0.85168332 0.62233305 0.84310108
		 0.37766692 0.84310108 0.64799631 0.86613703 0.62696636 0.86714637 0.37303367 0.86714637
		 0.35200366 0.86613703 0.67095608 0.86800271 0.32904392 0.86800271 0.60640621 0.87182069
		 0.39359382 0.87182069 0.57174504 0.88693941 0.58852708 0.88047218 0.42825493 0.88693941
		 0.41147295 0.88047218 0.55507135 0.89170814 0.44492862 0.89170814 0.75865328 0.85348302
		 0.24134675 0.85348302 0.75850332 0.87719923 0.24149665 0.87719923 0.5349828 0.87906015
		 0.21984568 0.85516942 0.53834665 0.89489925 0.46165332 0.89489925 0.74452424 0.46892989
		 0.78490251 0.46439508 0.25547576 0.46892995 0.21509761 0.46439511 0.80677718 0.49619368
		 0.19322294 0.49619371 0.72538722 0.58779174 0.2746129 0.58779198 0.75683999 0.59431416
		 0.24316013 0.59431434 0.78728306 0.58232296 0.21271706 0.58232307 0.81722641 0.55628341
		 0.18277365 0.55628353 0.81731439 0.52501488 0.1826857 0.52501494 0.79885346 0.42965704
		 0.20114666 0.42965701 0.80073857 0.40175211 0.19926152 0.40175211 0.79372603 0.37377876
		 0.20627397 0.37377873 0.77131641 0.34471592 0.22868365 0.34471586 0.72699898 0.34154573
		 0.27300113 0.3415457 0.6976366 0.35734737 0.30236357 0.3573474 0.71229446 0.61295319
		 0.28770554 0.61295348 0.69340336 0.64262867 0.30659676 0.64262897 0.66356426 0.67004555
		 0.33643574 0.67004603 0.85508704 0.55220461 0.14491308 0.55220461 0.88906759 0.54899538
		 0.11093244 0.54899544 0.85052955 0.52108997 0.14947054 0.52109003 0.84391475 0.49014446
		 0.15608516 0.49014443 0.83629215 0.45828444 0.16370803 0.45828447 0.83673406 0.42617777
		 0.16326603 0.42617774 0.83536041 0.39548495 0.16463968 0.39548492 0.83006203 0.36527738
		 0.16993809 0.36527732 0.8858093 0.51757729 0.1141907 0.51757729 0.88238728 0.48592985
		 0.11761275 0.48592985 0.87871587 0.45424414 0.12128419 0.45424411 0.87678635 0.42199823
		 0.12321356 0.4219982 0.87402642 0.38936993 0.1259737 0.3893699 0.86777169 0.35676688
		 0.13222843 0.35676679 0.92320013 0.54716569 0.076799929 0.54716575 0.92149031 0.51544756;
	setAttr ".uvst[0].uvsp[1250:1499]" 0.078509718 0.51544756 0.91989625 0.48357418
		 0.080103934 0.48357415 0.91823626 0.45176366 0.081763923 0.45176363 0.91656244 0.41890115
		 0.083437771 0.41890112 0.91425622 0.38331965 0.085743994 0.38331959 0.90932202 0.34046766
		 0.090678126 0.3404676 0.9573344 0.54629666 0.042665631 0.54629666 0.95674598 0.51431733
		 0.043253958 0.51431733 0.95626897 0.48252425 0.043730974 0.48252425 0.955827 0.4507789
		 0.044173002 0.4507789 0.9554345 0.41813955 0.044565558 0.41813949 0.95480466 0.38308465
		 0.04519555 0.38308463 0.95470142 0.34481525 0.045298547 0.34481519 0.99242282 0.54598939
		 0.99242294 0.51390696 0.99242294 0.48211673 0.99242294 0.45070708 0.99242294 0.4186134
		 0.99242294 0.38406533 0.99242294 0.34584829 0.36480564 0.36954451 0.37874645 0.38321599
		 0.63519448 0.36954442 0.62125367 0.38321593 0.39545459 0.38005146 0.60454559 0.38005137
		 0.41862586 0.37686217 0.58137417 0.37686211 0.44485638 0.37421229 0.55514371 0.37421224
		 0.47222432 0.3716951 0.5277757 0.3716951 0.50002569 0.37084436 0.38581806 0.35772234
		 0.61418217 0.35772225 0.41144884 0.35043046 0.58855122 0.3504304 0.43967289 0.34555051
		 0.56032717 0.34555045 0.4694618 0.34255841 0.53053826 0.34255838 0.50001341 0.34155411
		 0.34358126 0.3534281 0.65641874 0.35342801 0.37148905 0.33432549 0.62851113 0.33432537
		 0.40235591 0.32306236 0.59764421 0.3230623 0.43455213 0.31623995 0.56544787 0.31623989
		 0.46718335 0.31238872 0.53281677 0.31238872 0.50001836 0.31120062 0.67910194 0.33242083
		 0.32089821 0.33242089 0.35421044 0.30674961 0.64578974 0.30674952 0.3923704 0.29122034
		 0.60762972 0.29122022 0.42883587 0.28239119 0.57116419 0.28239113 0.4647921 0.27880713
		 0.53520799 0.27880704 0.50001711 0.2779963 0.75845349 0.21976227 0.29301059 0.30040553
		 0.23734951 0.20364496 0.76265049 0.20364496 0.23614365 0.18229863 0.76385635 0.18229863
		 0.42450649 0.24558656 0.57549363 0.24558643 0.46375445 0.24349603 0.50000507 0.24307229
		 0.53624564 0.24349582 0.50000519 0.20825462 0.45525068 0.21139914 0.23694399 0.089973509
		 0.0075771809 0.16559243 0.5447495 0.2113985 0.76305604 0.089973509 0.0075771809 0.21261603
		 0.96094084 0.21836491 0.039059192 0.21836482 0.95890188 0.30840567 0.96317768 0.27842081
		 0.99242294 0.27903742 0.0075771809 0.31069061 0.036822438 0.27842069 0.041098118
		 0.30840558 0.074431509 0.30193764 0.92556858 0.30193776 0.060023963 0.2781336 0.9399761
		 0.27813369 0.7668069 0.29168552 0.86667454 0.30359626 0.24284269 0.0060749501 0.24184264
		 0.27144474 0.064207911 0.2541323 0.93579227 0.25413239 0.22481836 0.027628154 0.22880933
		 0.0060749501 0.78429168 0.29168552 0.77518165 0.027628154 0.2209671 0.27230337 0.77903289
		 0.27230337 0.17676482 0.33581209 0.82323515 0.33581215 0.18042099 0.29945177 0.23887786
		 0.23664439 0.77868843 0.23568024 0.77850139 0.2536366 0.76102018 0.25521198 0.76671278
		 0.29851466 0.22252515 0.21863917 0.22282286 0.20075858 0.77717716 0.20075858 0.77747488
		 0.21863917 0.22367629 0.17919594 0.77632368 0.17919594 0.22399646 0.056163289 0.77600354
		 0.056163289 0.22303481 0.090493351 0.7769652 0.090493351 0.22330678 0.12840343 0.77669322
		 0.12840343 0.22445589 0.15672036 0.77554411 0.15672036 0.22149864 0.2536366 0.22131154
		 0.23568024 0.85521865 0.33458015 0.14478129 0.33458009 0.21040015 0.030493416 0.21225059
		 0.0060749501 0.79889584 0.29168555 0.78959984 0.030493416 0.20393392 0.27219033 0.79606605
		 0.27219033 0.20483997 0.25271356 0.20499662 0.23413153 0.79500341 0.23413153 0.79516006
		 0.25271356 0.20561026 0.21984778 0.20696747 0.1993911 0.79303253 0.1993911 0.79438972
		 0.21984778 0.20820844 0.17836583 0.79179156 0.17836583 0.20984934 0.059738211 0.79015064
		 0.059738211 0.21119986 0.095413759 0.78880012 0.095413759 0.21094494 0.13097456 0.78905505
		 0.13097456 0.21011715 0.15563068 0.78988284 0.15563068 0.0075771809 0.24777578 0.96150059
		 0.24976201 0.038499475 0.24976194 0.69393981 0.27934289 0.70940667 0.2910744 0.81754184
		 0.17486744 0.81537485 0.15799046 0.27462757 0.27939373 0.29059333 0.2910744 0.18245816
		 0.17486744 0.18316001 0.1917107 0.72537243 0.27939373 0.81683999 0.1917107 0.30606019
		 0.27934289 0.18462513 0.15799046 0.80890667 0.17580265 0.80710995 0.15689026 0.19289008
		 0.15689026 0.19109331 0.17580265 0.80980003 0.1946733 0.19019997 0.1946733 0.82677734
		 0.14569269 0.8150692 0.14579198 0.31850058 0.28197509 0.1849308 0.14579198 0.66716313
		 0.26490143 0.81475002 0.12678996 0.33283687 0.26490143 0.18525 0.12678996 0.80441427
		 0.13734359 0.19558573 0.13734359 0.74137795 0.27965769 0.81811988 0.20324466 0.17322268
		 0.20428917 0.18188009 0.20324466 0.81072962 0.21163687 0.18927038 0.21163687 0.817406
		 0.21949694 0.81063312 0.23046979 0.182594 0.21949694 0.18936688 0.23046979 0.8029812
		 0.097932771 0.19701882 0.097932771 0.75229704 0.26314244 0.24770297 0.26314244 0.66698849
		 0.24430744 0.33301151 0.24430744 0.75198758 0.24504346 0.24801244 0.24504346 0.74942988
		 0.22380024 0.80996901 0.25161034 0.25057012 0.22380024 0.19003099 0.25161034 0.73651564
		 0.20038098 0.80972648 0.27211875 0.26348439 0.20038098 0.19027355 0.27211875 0.80225503
		 0.0060749501 0.19774497 0.0060749501 0.71314299 0.19138221 0.28685701 0.19138221
		 0.80295217 0.031937912 0.19704783 0.031937912 0.68843651 0.19698665 0.31156349 0.19698665
		 0.80270839 0.064304352 0.19729164 0.064304352 0.67072177 0.21948136 0.32927823 0.21948136
		 0.71383148 0.21210992 0.72844994 0.21473752 0.28616852 0.21210992 0.27155009 0.21473752
		 0.69901747 0.21231341 0.30098253 0.21231341 0.71473753 0.26931393 0.73386449 0.26564848
		 0.28526247 0.26931393 0.26613551 0.26564848 0.69722527 0.26851261 0.30277473 0.26851261
		 0.69540465 0.24748026;
	setAttr ".uvst[0].uvsp[1500:1749]" 0.71361125 0.24815917 0.30459535 0.24748026
		 0.28638875 0.24815917 0.7328198 0.24678753 0.2671802 0.24678753 0.73113883 0.22846311
		 0.71325898 0.22907284 0.26886114 0.22846311 0.28674102 0.22907284 0.69442207 0.22745673
		 0.30557793 0.22745673 0.49398065 0.49596578 0.50001884 0.49656874 0.50601941 0.49596497
		 0.48861948 0.49417478 0.51138079 0.49417537 0.48520666 0.48826835 0.5147934 0.48826838
		 0.48498291 0.48241329 0.51501721 0.48241332 0.48632306 0.47612253 0.51367688 0.47612253
		 0.48948848 0.47204429 0.5105117 0.47204426 0.49434006 0.46968248 0.50566018 0.46968246
		 0.5000236 0.46893141 0.50002289 0.47796142 0.49544299 0.47793216 0.49551338 0.47417045
		 0.50001884 0.47382784 0.50455713 0.47793216 0.50448674 0.47417042 0.50001484 0.48263067
		 0.49526739 0.48260242 0.50473273 0.48260242 0.50001925 0.48687261 0.49534982 0.48689532
		 0.50465029 0.48689532 0.49524826 0.49115333 0.50001723 0.49126109 0.50475186 0.49115342
		 0.49161741 0.49067989 0.5083828 0.49067974 0.49082166 0.48716977 0.50917822 0.4871698
		 0.49056077 0.4827041 0.50943935 0.4827041 0.49118498 0.477633 0.50881505 0.477633
		 0.49215624 0.47469464 0.50784385 0.47469461 0.56010699 0.55445993 0.56701964 0.55443525
		 0.56602347 0.56157798 0.56084335 0.55945873 0.43289164 0.53070891 0.43244982 0.53859711
		 0.42515138 0.53778172 0.42566067 0.52869284 0.55916506 0.51672012 0.55062419 0.52550298
		 0.57060379 0.51113683 0.58127689 0.50946766 0.59612566 0.51203394 0.60605556 0.5176698
		 0.61540127 0.52945745 0.61849415 0.54922539 0.61539251 0.55992156 0.6067909 0.57170832
		 0.59351027 0.58063579 0.5802443 0.58262706 0.56873059 0.58045578 0.55799598 0.57425177
		 0.54975277 0.56561327 0.54591632 0.55827355 0.54399109 0.54845464 0.54543412 0.53670055
		 0.44937581 0.52550298 0.44083488 0.51672018 0.45456594 0.53670073 0.45600909 0.5484547
		 0.45408371 0.55827355 0.45024717 0.56561339 0.44200402 0.57425195 0.43126947 0.58045596
		 0.4197557 0.58262724 0.40648985 0.58063596 0.39320916 0.57170844 0.38460761 0.55992174
		 0.38150591 0.54922551 0.38459885 0.52945763 0.39394456 0.51766992 0.40387446 0.51203406
		 0.41872296 0.50946784 0.42939633 0.51113701 0.56425762 0.52298868 0.55707705 0.52970707
		 0.5732137 0.51905954 0.58134931 0.51805919 0.59246087 0.51995343 0.60015583 0.52425081
		 0.60750657 0.53325772 0.610439 0.54851222 0.60787314 0.55812192 0.60123575 0.5684582
		 0.59120262 0.57239312 0.58141565 0.57356727 0.571338 0.5721522 0.56230831 0.56838864
		 0.55608481 0.56243455 0.55308723 0.55602157 0.55171025 0.54812944 0.55272144 0.53874367
		 0.44292295 0.52970725 0.43574265 0.5229888 0.44727862 0.53874379 0.44828987 0.5481295
		 0.44691274 0.55602163 0.44391525 0.56243461 0.43769175 0.56838876 0.428662 0.57215232
		 0.41858447 0.57356745 0.4087975 0.5723933 0.39876437 0.56845844 0.3921271 0.5581221
		 0.38956112 0.54851234 0.39249343 0.53325796 0.39984405 0.52425092 0.4075391 0.51995367
		 0.41865069 0.51805931 0.42678642 0.51905966 0.56710839 0.53070879 0.56203043 0.53334451
		 0.57433939 0.52869266 0.58205897 0.52795559 0.59005058 0.52868021 0.59523088 0.52984101
		 0.59817553 0.53694433 0.59936398 0.54731256 0.59798664 0.55696064 0.59569776 0.56365591
		 0.58977836 0.56402308 0.58179516 0.56411219 0.57344627 0.5632602 0.55992508 0.54729879
		 0.56060588 0.53946352 0.43796957 0.53334469 0.4393943 0.5394637 0.44007492 0.54729891
		 0.43989301 0.55446005 0.43915671 0.55945885 0.43397683 0.56157804 0.42655379 0.56326038
		 0.41820496 0.56411231 0.41022176 0.56402326 0.40430224 0.56365609 0.40201348 0.55696076
		 0.40063602 0.5473128 0.40182459 0.53694451 0.40476924 0.52984124 0.40994951 0.52868038
		 0.41794115 0.52795577 0.5675503 0.53859687 0.56735951 0.54669517 0.42556179 0.55489177
		 0.42515337 0.5464772 0.43264049 0.54669523 0.43298036 0.55443537 0.59038574 0.53744227
		 0.58263069 0.54651302 0.58252078 0.53740394 0.59067398 0.54677522 0.58236206 0.5553956
		 0.59022713 0.55580592 0.57443821 0.55489171 0.57484663 0.54647696 0.57484889 0.5377816
		 0.41763806 0.55539578 0.41736937 0.54651308 0.41747922 0.53740412 0.40977287 0.55580604
		 0.40932608 0.5467754 0.40961438 0.53744245 0.55510247 0.63692576 0.52743411 0.63752419
		 0.47256607 0.63752437 0.44489765 0.63692605 0.50001138 0.63749838 0.99242294 0.24777587
		 0.0075771809 0.27903733 0.99242294 0.2126161 0.99242294 0.31069073 0.0075771213 0.3458482
		 0.0075771809 0.3840653 0.0075771809 0.41861337 0.0075771809 0.45070705 0.0075771809
		 0.48211673 0.0075771809 0.5139069 0.0075771213 0.54598933 0.0075771213 0.5786702
		 0.0075771809 0.61631984 0.0075771809 0.66019088 0.0075771809 0.71220022 0.0075771809
		 0.77754909 0.38295981 0.99327087 0.3634747 0.99327087 0.61704016 0.99327087 0.60020947
		 0.99327087 0.43556008 0.99327087 0.22158486 0.99327087 0.56443989 0.99327087 0.55111492
		 0.99327087 0.26219502 0.99327087 0.2862412 0.99327087 0.73780501 0.99327087 0.71375883
		 0.99327087 0.33001566 0.99327087 0.34770539 0.99327087 0.66998434 0.99327087 0.65229464
		 0.99327087 0.39979056 0.99327087 0.63652527 0.99327087 0.30907828 0.99327087 0.69092172
		 0.99327087 0.24098706 0.99327087 0.75901294 0.99327087 0.41992107 0.99327087 0.58007896
		 0.99327087 0.32679981 0.0066586775 0.34794837 0.0066587371 0.26803821 0.00665867
		 0.4768129 0.0066587445 0.43842715 0.0066587445 0.41712481 0.0066587445 0.38320988
		 0.0066587371 0.3654964 0.0066587371 0.30420184 0.0066587296 0.40061867 0.0066587371
		 0.45614344 0.0066587445 0.28383934 0.0066587296 0.6345036 0.0066587371 0.65205163
		 0.0066587371 0.61679012 0.0066587371 0.59938133 0.0066587371 0.58287519 0.0066587445
		 0.56157285 0.0066587445 0.54385656 0.0066587445 0.74654198 0.0066587296 0.73196179
		 0.00665867;
	setAttr ".uvst[0].uvsp[1750:1960]" 0.71616066 0.0066587296 0.69579816 0.0066587296
		 0.67320019 0.0066586775 0.13332561 0.30359617 0.2389798 0.25521198 0.81957906 0.29945189
		 0.75815737 0.27144474 0.23328745 0.29851454 0.24154654 0.21976227 0.70698965 0.30040547
		 0.76112211 0.23664439 0.76227129 0.02048704 0.76204848 0.047304109 0.23795149 0.047304109
		 0.23772873 0.02048704 0.90140474 0.28135687 0.098595321 0.28135678 0.47576576 0.16188797
		 0.030828685 0.16515972 0.9691714 0.16515969 0.52423429 0.16188341 0.4999938 0.16154197
		 0.99242294 0.1655924 0.76321775 0.13331683 0.23678225 0.13331683 0.76064521 0.16025023
		 0.23935477 0.16025023 0.61699098 0.25911832 0.38300902 0.25911838 0.65889132 0.27786753
		 0.3411088 0.27786767 0.29556155 0.70524752 0.70443857 0.7052471 0.33957469 0.73536927
		 0.79786861 0.76836127 0.44230139 0.77146131 0.38856584 0.75804389 0.61143422 0.75804257
		 0.91190207 0.84758037 0.50000328 0.77185404 0.0075771213 0.86696011 0.47746313 0.099323288
		 0.24588425 0.095346212 0.24496301 0.11345267 0.24498446 0.13056952 0.4488911 0.18324752
		 0.25001001 0.15788987 0.25256586 0.16702279 0.31894463 0.1660281 0.31449449 0.17013349
		 0.35233372 0.18232723 0.39036292 0.2068053 0.28381157 0.17454106 0.25566089 0.17366564
		 0.39288491 0.21381773 0.26553756 0.1828151 0.41067231 0.22781099 0.27339029 0.18085167
		 0.40034986 0.22589482 0.41691881 0.22463909 0.3032766 0.1797532 0.35344845 0.20144813
		 0.37918973 0.20788567 0.28730744 0.18109417 0.36076832 0.21055581 0.2958262 0.18395329
		 0.28985476 0.18322414 0.32341361 0.16319759 0.32744235 0.1608599 0.25255036 0.0391801
		 0.47947311 0.029317956 0.25345802 0.0066587296 0.2499931 0.05734574 0.53377712 0.14476763
		 0.75411576 0.095346212 0.75232738 0.07637208 0.54208869 0.16531055 0.5511089 0.18324752
		 0.5603416 0.19731751 0.56906354 0.20841214 0.68550551 0.17013349 0.68105537 0.1660281
		 0.69175023 0.17537279 0.61556286 0.2046489 0.72018355 0.1736744 0.58308119 0.22463909
		 0.74433911 0.17366564 0.72660971 0.18085167 0.60711509 0.21381773 0.59965014 0.22589482
		 0.73446244 0.1828151 0.58932769 0.22781099 0.6967234 0.1797532 0.62081027 0.20788567
		 0.71014524 0.18322414 0.63923168 0.21055581 0.71269256 0.18109417 0.7041738 0.18395329
		 0.64655155 0.20144813 0.66465241 0.15799524 0.67658639 0.16319759 0.52052689 0.029317956
		 0.5231871 0.0066587445 0.51969957 0.052439824 0.52050006 0.075828396 0.21994042 0.87882543
		 0.46501717 0.87906015 0.78005958 0.87882543 0.54222703 0.91203284 0.2213144 0.90326661
		 0.78015435 0.85516942 0.46747836 0.86501545 0.77941835 0.83276635 0.22223976 0.81305987
		 0.77776027 0.81305987 0.46744511 0.84508598 0.77594078 0.79450154 0.46746328 0.83851892
		 0.77357805 0.77496374 0.42279157 0.79733491 0.42850998 0.80096495 0.57149005 0.80096495
		 0.5772084 0.79733491 0.33210614 0.75921679 0.56219316 0.79958773 0.27154243 0.75245786
		 0.45696566 0.81910253 0.72150362 0.75076175 0.54077315 0.82174426 0.2274448 0.76004744
		 0.47077963 0.83394551 0.46642742 0.82226223 0.2559697 0.73731965 0.77255523 0.76004744
		 0.53357255 0.82226223 0.24058732 0.73573464 0.23431602 0.7397759 0.75941265 0.73573464
		 0.52762663 0.83224493 0.24749932 0.73478305 0.52673876 0.82730645 0.74403036 0.73731965
		 0.52922034 0.83394551 0.32554021 0.74374664 0.3181541 0.73202729 0.45957693 0.81544197
		 0.46181789 0.81159765 0.67445982 0.74374664 0.54042304 0.81544197 0.46144769 0.80308163
		 0.45763519 0.79951787 0.53855228 0.80308163 0.68837005 0.72521019 0.46274093 0.80717587
		 0.53818214 0.81159765 0.7074796 0.72411728 0.6818459 0.73202729 0.36866179 0.78588223
		 0.40762731 0.78965437 0.63133824 0.78588223 0.59852445 0.7900275 0.39062741 0.79108375
		 0.59237266 0.78965437 0.45001325 0.96939719 0.44888505 0.99327087 0.54998672 0.96939719
		 0.77841508 0.99327087 0.45099661 0.94862235 0.77396631 0.94983178 0.45392808 0.92986202
		 0.77635109 0.92720574 0.33000302 0.15868761 0.66999698 0.15868761 0.17322269 0.098233297
		 0.17322269 0.068609744 0.82677734 0.068609744 0.82677734 0.098233297 0.17322269 0.035498336
		 0.82677734 0.035498336 0.17322266 0.0060749799 0.17322272 0.29168555 0.82677728 0.29168555
		 0.82677734 0.0060749799 0.17322272 0.27207398 0.19008908 0.29168555 0.80991089 0.29168555
		 0.82677728 0.27207398 0.20110413 0.29168555 0.78774941 0.0060749501 0.17322266 0.25060374
		 0.82677734 0.25060374 0.17322268 0.23367411 0.82677734 0.23367411 0.17322269 0.21929942
		 0.82677734 0.21929942 0.17322269 0.12182879 0.82677734 0.12182879 0.77119064 0.0060749501
		 0.21570833 0.29168552 0.75715733 0.0060749501 0.2331931 0.29168552 0.25862205 0.27965769
		 0.17322269 0.19206189 0.17322269 0.15777016 0.17322268 0.14569269 0.17322265 0.17494197
		 0.82677734 0.19206189 0.82677734 0.20428917 0.68149942 0.28197509 0.82677734 0.15777016
		 0.82677734 0.17494197;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 454 ".pt";
	setAttr ".pt[0]" -type "float3" 0.42525291 -0.010389328 -1.8652804 ;
	setAttr ".pt[1]" -type "float3" 0.79425621 -0.041124344 -1.8476244 ;
	setAttr ".pt[2]" -type "float3" 1.0857124 -0.044214249 -2.4913924 ;
	setAttr ".pt[3]" -type "float3" 0.64944267 -0.013118744 -2.5150309 ;
	setAttr ".pt[4]" -type "float3" 1.2415276 -0.056192398 -2.4243982 ;
	setAttr ".pt[5]" -type "float3" 0.92134857 -0.051959991 -1.8141918 ;
	setAttr ".pt[6]" -type "float3" 0.88357162 -0.050020218 -1.7258655 ;
	setAttr ".pt[7]" -type "float3" 1.2213459 -0.056331635 -2.3101404 ;
	setAttr ".pt[8]" -type "float3" 0.6185894 -0.0293293 -1.6472423 ;
	setAttr ".pt[9]" -type "float3" 0.89503098 -0.034546852 -2.2123649 ;
	setAttr ".pt[10]" -type "float3" 0.31790924 0.0055894852 -2.1719885 ;
	setAttr ".pt[11]" -type "float3" 0.18400192 0.0059099197 -1.5923092 ;
	setAttr ".pt[12]" -type "float3" -0.39221001 0.05384922 -1.5707417 ;
	setAttr ".pt[13]" -type "float3" -0.48257446 0.062438965 -2.2304406 ;
	setAttr ".pt[14]" -type "float3" -1.0110378 0.10727406 -1.6534023 ;
	setAttr ".pt[15]" -type "float3" -1.1611576 0.11125469 -2.3687556 ;
	setAttr ".pt[16]" -type "float3" -1.3681412 0.12727356 -2.4860759 ;
	setAttr ".pt[17]" -type "float3" -1.2077503 0.12540627 -1.7631102 ;
	setAttr ".pt[18]" -type "float3" -1.0338078 0.11137009 -1.8386836 ;
	setAttr ".pt[19]" -type "float3" -1.2009544 0.11708069 -2.5594888 ;
	setAttr ".pt[20]" -type "float3" -0.57860184 0.073280334 -1.8628802 ;
	setAttr ".pt[21]" -type "float3" -0.71113586 0.083465576 -2.5876613 ;
	setAttr ".pt[22]" -type "float3" -0.042856216 0.03635788 -2.5661578 ;
	setAttr ".pt[23]" -type "float3" -0.064165115 0.030334473 -1.8733109 ;
	setAttr ".pt[24]" -type "float3" 0.35588264 -0.012081146 -1.3930018 ;
	setAttr ".pt[25]" -type "float3" 0.66164207 -0.038414001 -1.4080535 ;
	setAttr ".pt[26]" -type "float3" 0.77126122 -0.048288345 -1.3813186 ;
	setAttr ".pt[27]" -type "float3" 0.73586273 -0.046327591 -1.2990186 ;
	setAttr ".pt[28]" -type "float3" 0.52217674 -0.029136658 -1.1986817 ;
	setAttr ".pt[29]" -type "float3" 0.14819145 0.0020074844 -1.1013197 ;
	setAttr ".pt[30]" -type "float3" -0.37097931 0.045862198 -1.0121846 ;
	setAttr ".pt[31]" -type "float3" -0.92765427 0.093902588 -0.99203014 ;
	setAttr ".pt[32]" -type "float3" -1.1094456 0.11102295 -1.0915313 ;
	setAttr ".pt[33]" -type "float3" -0.99256325 0.10222626 -1.1912866 ;
	setAttr ".pt[34]" -type "float3" -0.56773949 0.066703796 -1.2911303 ;
	setAttr ".pt[35]" -type "float3" -0.094764709 0.026445389 -1.3496007 ;
	setAttr ".pt[36]" -type "float3" 0.29744911 -0.015769958 -0.74406374 ;
	setAttr ".pt[37]" -type "float3" 0.66370773 -0.046302795 -0.83694959 ;
	setAttr ".pt[38]" -type "float3" 0.80805588 -0.058813095 -0.83830523 ;
	setAttr ".pt[39]" -type "float3" 0.85336494 -0.062446594 -0.86042213 ;
	setAttr ".pt[40]" -type "float3" 0.5773735 -0.040273666 -0.72851241 ;
	setAttr ".pt[41]" -type "float3" 0.18312836 -0.0077476501 -0.6033355 ;
	setAttr ".pt[42]" -type "float3" -0.3177166 0.034273148 -0.49621201 ;
	setAttr ".pt[43]" -type "float3" -0.75374794 0.071796417 -0.47250938 ;
	setAttr ".pt[44]" -type "float3" -0.89448547 0.084653854 -0.52023268 ;
	setAttr ".pt[45]" -type "float3" -0.83651924 0.080755234 -0.60408878 ;
	setAttr ".pt[46]" -type "float3" -0.44619179 0.046722412 -0.59246731 ;
	setAttr ".pt[47]" -type "float3" -0.083938599 0.016410828 -0.67599368 ;
	setAttr ".pt[48]" -type "float3" 0.051620483 -0.0037136078 -0.056813002 ;
	setAttr ".pt[49]" -type "float3" 0.10458565 -0.0083732605 -0.052077055 ;
	setAttr ".pt[50]" -type "float3" 0.29293442 -0.023920059 -0.11134768 ;
	setAttr ".pt[51]" -type "float3" 0.53276253 -0.043272018 -0.21965766 ;
	setAttr ".pt[52]" -type "float3" 0.49153709 -0.040075302 -0.19139636 ;
	setAttr ".pt[53]" -type "float3" 0.18637466 -0.014865875 -0.096937314 ;
	setAttr ".pt[54]" -type "float3" -0.20289421 0.017608643 5.6505203e-05 ;
	setAttr ".pt[55]" -type "float3" -0.48163414 0.041223526 0.04271245 ;
	setAttr ".pt[56]" -type "float3" -0.65014267 0.056566238 -0.010409355 ;
	setAttr ".pt[57]" -type "float3" -0.49041176 0.043296814 -0.054369926 ;
	setAttr ".pt[58]" -type "float3" -0.18303108 0.016702652 -0.060630322 ;
	setAttr ".pt[59]" -type "float3" -0.013912201 0.001657486 -0.033357382 ;
	setAttr ".pt[60]" -type "float3" 1.2098427 -0.0091552734 -5.5183868 ;
	setAttr ".pt[61]" -type "float3" 1.4609108 -0.029743195 -5.2778373 ;
	setAttr ".pt[62]" -type "float3" 1.3862572 -0.027357101 -5.0790443 ;
	setAttr ".pt[63]" -type "float3" 1.0808487 -0.00071144104 -5.4873757 ;
	setAttr ".pt[64]" -type "float3" 1.4752846 -0.035568237 -4.9277768 ;
	setAttr ".pt[65]" -type "float3" 1.1773453 -0.017375946 -4.7614594 ;
	setAttr ".pt[66]" -type "float3" 1.0256424 -0.01156044 -4.4273181 ;
	setAttr ".pt[67]" -type "float3" 1.383255 -0.033540726 -4.6163707 ;
	setAttr ".pt[68]" -type "float3" 0.73761749 0.015558243 -4.9565687 ;
	setAttr ".pt[69]" -type "float3" 0.54353714 0.025211334 -4.6891084 ;
	setAttr ".pt[70]" -type "float3" 0.33072281 0.046806335 -5.1929803 ;
	setAttr ".pt[71]" -type "float3" 0.49879456 0.038011551 -5.3929663 ;
	setAttr ".pt[72]" -type "float3" 0.70010757 0.027526855 -5.6360927 ;
	setAttr ".pt[73]" -type "float3" 0.49236298 0.041151047 -5.5882664 ;
	setAttr ".pt[74]" -type "float3" 0.97898102 0.0082454681 -5.6286492 ;
	setAttr ".pt[75]" -type "float3" 0.85668945 0.01673317 -5.6343355 ;
	setAttr ".pt[76]" -type "float3" 0.72504425 0.025661469 -5.6252851 ;
	setAttr ".pt[77]" -type "float3" 1.0851555 0.0024394989 -5.736896 ;
	setAttr ".pt[78]" -type "float3" 1.0589104 0.0048999786 -5.7844286 ;
	setAttr ".pt[79]" -type "float3" 0.89891052 0.0154953 -5.7548294 ;
	setAttr ".pt[80]" -type "float3" 1.3510742 -0.016702652 -5.6750879 ;
	setAttr ".pt[81]" -type "float3" 1.4727859 -0.026327133 -5.5843 ;
	setAttr ".pt[82]" -type "float3" 1.5094643 -0.030605316 -5.457159 ;
	setAttr ".pt[83]" -type "float3" 1.3127365 -0.01471138 -5.6284132 ;
	setAttr ".pt[84]" -type "float3" 1.3464355 -0.016054153 -5.6988621 ;
	setAttr ".pt[85]" -type "float3" 1.2292595 -0.0073032379 -5.7490335 ;
	setAttr ".pt[86]" -type "float3" 1.5563431 -0.037382126 -5.2000036 ;
	setAttr ".pt[87]" -type "float3" 1.5449142 -0.033607483 -5.4163032 ;
	setAttr ".pt[88]" -type "float3" 1.4569626 -0.02728653 -5.4360151 ;
	setAttr ".pt[89]" -type "float3" 1.3608437 -0.02604866 -5.0472031 ;
	setAttr ".pt[90]" -type "float3" 1.1444664 -0.0051727295 -5.4811831 ;
	setAttr ".pt[91]" -type "float3" 0.93827057 0.0052852631 -5.2117639 ;
	setAttr ".pt[92]" -type "float3" 1.10429 0.00094413757 -5.7239408 ;
	setAttr ".pt[93]" -type "float3" 0.99764252 0.028579712 -7.1935 ;
	setAttr ".pt[94]" -type "float3" 1.0061913 0.022989273 -6.8312817 ;
	setAttr ".pt[95]" -type "float3" 1.1277313 0.013341904 -6.7380962 ;
	setAttr ".pt[96]" -type "float3" 1.155426 0.017255783 -7.1591506 ;
	setAttr ".pt[97]" -type "float3" 0.62625885 0.050327301 -6.9189725 ;
	setAttr ".pt[98]" -type "float3" 0.69594193 0.050020218 -7.2435789 ;
	setAttr ".pt[99]" -type "float3" 0.40463257 0.070665359 -7.2878494 ;
	setAttr ".pt[100]" -type "float3" 0.22268677 0.078952789 -6.9820662 ;
	setAttr ".pt[101]" -type "float3" 0.30516815 0.077138901 -7.2612729 ;
	setAttr ".pt[102]" -type "float3" 0.1181221 0.085613251 -6.943635 ;
	setAttr ".pt[103]" -type "float3" 0.67563248 0.050310135 -7.1635103 ;
	setAttr ".pt[104]" -type "float3" 0.64142227 0.04813385 -6.8356237 ;
	setAttr ".pt[105]" -type "float3" 0.30571747 0.071882248 -6.8835106 ;
	setAttr ".pt[106]" -type "float3" 0.43116379 0.067512512 -7.1916895 ;
	setAttr ".pt[107]" -type "float3" 1.0106392 0.026874542 -7.1347375 ;
	setAttr ".pt[108]" -type "float3" 0.97787094 0.023893356 -6.755837 ;
	setAttr ".pt[109]" -type "float3" 0.97132492 0.033353806 -7.4080777 ;
	setAttr ".pt[110]" -type "float3" 0.92058563 0.036237717 -7.3642297 ;
	setAttr ".pt[111]" -type "float3" 0.51176453 0.065092087 -7.4177179 ;
	setAttr ".pt[112]" -type "float3" 0.53866196 0.063100815 -7.407527 ;
	setAttr ".pt[113]" -type "float3" 0.73244476 0.050519943 -7.4614573 ;
	setAttr ".pt[114]" -type "float3" 0.72512817 0.050258636 -7.4060688 ;
	setAttr ".pt[115]" -type "float3" 0.94607925 0.034627914 -7.3745332 ;
	setAttr ".pt[116]" -type "float3" 0.71285248 0.050941467 -7.3944235 ;
	setAttr ".pt[117]" -type "float3" 0.53628922 0.062587738 -7.3584919 ;
	setAttr ".pt[118]" -type "float3" 0.95503998 0.020271301 -6.3799257 ;
	setAttr ".pt[119]" -type "float3" 1.0196381 0.015378952 -6.3474131 ;
	setAttr ".pt[120]" -type "float3" 0.88815308 0.024652481 -6.3641853 ;
	setAttr ".pt[121]" -type "float3" 0.59778976 0.047204971 -6.551208 ;
	setAttr ".pt[122]" -type "float3" 0.23330688 0.071176529 -6.4720311 ;
	setAttr ".pt[123]" -type "float3" 0.010520935 0.087202072 -6.5229759 ;
	setAttr ".pt[124]" -type "float3" 0.078987122 0.082756042 -6.5420351 ;
	setAttr ".pt[125]" -type "float3" 0.50051117 0.052959442 -6.48352 ;
	setAttr ".pt[126]" -type "float3" 1.171463 0.0039491653 -6.2758217 ;
	setAttr ".pt[127]" -type "float3" 1.1969147 0.0082845688 -6.7163868 ;
	setAttr ".pt[128]" -type "float3" 1.246685 0.0046405792 -6.7003565 ;
	setAttr ".pt[129]" -type "float3" 1.166153 0.0043783188 -6.2804704 ;
	setAttr ".pt[130]" -type "float3" 1.0209503 0.020675659 -6.7373166 ;
	setAttr ".pt[131]" -type "float3" 0.68069839 0.045084 -6.8103657 ;
	setAttr ".pt[132]" -type "float3" 0.63071442 0.044009209 -6.4837832 ;
	setAttr ".pt[133]" -type "float3" 0.89487457 0.023628235 -6.3234558 ;
	setAttr ".pt[134]" -type "float3" 0.41863632 0.062912941 -6.7964382 ;
	setAttr ".pt[135]" -type "float3" 0.29279709 0.066418648 -6.4237461 ;
	setAttr ".pt[136]" -type "float3" 0.32324982 0.069320679 -6.7853904 ;
	setAttr ".pt[137]" -type "float3" 0.43157959 0.0617342 -6.775538 ;
	setAttr ".pt[138]" -type "float3" 0.28796387 0.066227913 -6.3859215 ;
	setAttr ".pt[139]" -type "float3" 0.1436615 0.076726913 -6.4275198 ;
	setAttr ".pt[140]" -type "float3" 0.74856186 0.03963089 -6.7534852 ;
	setAttr ".pt[141]" -type "float3" 0.68302536 0.03834343 -6.3340812 ;
	setAttr ".pt[142]" -type "float3" 1.0086288 0.029776573 -7.334795 ;
	setAttr ".pt[143]" -type "float3" 1.2009277 0.013145447 -7.0882015 ;
	setAttr ".pt[144]" -type "float3" 1.1308746 0.018621445 -7.1358633 ;
	setAttr ".pt[145]" -type "float3" 0.99148941 0.030772209 -7.3215523 ;
	setAttr ".pt[146]" -type "float3" 0.63799667 0.053565979 -7.2117844 ;
	setAttr ".pt[147]" -type "float3" 0.62285995 0.054862976 -7.2303133 ;
	setAttr ".pt[148]" -type "float3" 0.78319168 0.045395851 -7.343184 ;
	setAttr ".pt[149]" -type "float3" 0.78491974 0.044534683 -7.2894616 ;
	setAttr ".pt[150]" -type "float3" 0.55458832 0.057217598 -7.0609288 ;
	setAttr ".pt[151]" -type "float3" 0.48593521 0.06188488 -7.0569735 ;
	setAttr ".pt[152]" -type "float3" 0.77296829 0.042609215 -7.0906067 ;
	setAttr ".pt[153]" -type "float3" 1.0163269 0.025898933 -7.0924397 ;
	setAttr ".pt[154]" -type "float3" 0.95388412 0.032758713 -7.2781038 ;
	setAttr ".pt[155]" -type "float3" 0.74946976 0.047182083 -7.3046055 ;
	setAttr ".pt[156]" -type "float3" 0.72244644 0.046320915 -7.1077461 ;
	setAttr ".pt[157]" -type "float3" 0.61430359 0.055288315 -7.2185287 ;
	setAttr ".pt[158]" -type "float3" 0.52600479 0.059300423 -7.0694332 ;
	setAttr ".pt[159]" -type "float3" 0.91139221 0.016572952 -5.8949704 ;
	setAttr ".pt[160]" -type "float3" 0.38100815 0.053806305 -5.9499025 ;
	setAttr ".pt[161]" -type "float3" -0.039241791 0.084005356 -6.043992 ;
	setAttr ".pt[162]" -type "float3" 1.3554115 -0.038694382 -4.104732 ;
	setAttr ".pt[163]" -type "float3" 1.4100533 -0.032283783 -4.8408155 ;
	setAttr ".pt[164]" -type "float3" 1.086216 0.0044612885 -5.8885307 ;
	setAttr ".pt[165]" -type "float3" 1.0959053 0.0038814545 -5.8947978 ;
	setAttr ".pt[166]" -type "float3" 1.1974182 -0.0096855164 -5.4181066 ;
	setAttr ".pt[167]" -type "float3" 1.220192 -0.01117897 -5.423419 ;
	setAttr ".pt[168]" -type "float3" 1.3842735 -0.032632828 -4.6872053 ;
	setAttr ".pt[169]" -type "float3" 1.4694042 -0.048877716 -3.9350593 ;
	setAttr ".pt[170]" -type "float3" 1.1266136 0.0014133453 -5.8689814 ;
	setAttr ".pt[171]" -type "float3" 1.1576958 -0.0017738342 -5.7930679 ;
	setAttr ".pt[172]" -type "float3" 1.1362343 -0.00029182434 -5.7934766 ;
	setAttr ".pt[173]" -type "float3" 0.65802765 0.033133507 -5.8325028 ;
	setAttr ".pt[174]" -type "float3" 0.11262512 0.071838379 -5.9192038 ;
	setAttr ".pt[175]" -type "float3" -0.1032753 0.087507248 -5.9786201 ;
	setAttr ".pt[176]" -type "float3" -0.1255188 0.089888573 -6.0403576 ;
	setAttr ".pt[177]" -type "float3" -0.14497757 0.09145546 -6.0568423 ;
	setAttr ".pt[178]" -type "float3" -0.15350342 0.09188652 -6.0456319 ;
	setAttr ".pt[179]" -type "float3" 0.13498306 0.0651474 -5.5461259 ;
	setAttr ".pt[180]" -type "float3" -0.23058701 0.090251923 -5.5436077 ;
	setAttr ".pt[181]" -type "float3" 0.0080986023 0.066249847 -4.9943018 ;
	setAttr ".pt[182]" -type "float3" 0.15284729 0.045808792 -4.2352667 ;
	setAttr ".pt[183]" -type "float3" -0.41621017 0.10296822 -5.5400038 ;
	setAttr ".pt[184]" -type "float3" -0.4097023 0.10343933 -5.6065049 ;
	setAttr ".pt[185]" -type "float3" -0.4251709 0.095178604 -4.9315314 ;
	setAttr ".pt[186]" -type "float3" -0.52419662 0.091854095 -4.1980591 ;
	setAttr ".pt[187]" -type "float3" -0.69281006 0.11314964 -4.900126 ;
	setAttr ".pt[188]" -type "float3" -0.88817787 0.11640549 -4.1633329 ;
	setAttr ".pt[189]" -type "float3" -0.75326538 0.11686325 -4.8679395 ;
	setAttr ".pt[190]" -type "float3" -0.38007736 0.099286079 -5.4533906 ;
	setAttr ".pt[191]" -type "float3" -0.64025116 0.10765457 -4.7639894 ;
	setAttr ".pt[192]" -type "float3" -0.060424805 0.075680733 -5.3359108 ;
	setAttr ".pt[193]" -type "float3" -0.18225098 0.074207306 -4.6228318 ;
	setAttr ".pt[194]" -type "float3" 0.61131668 0.027989388 -5.2276173 ;
	setAttr ".pt[195]" -type "float3" 0.542099 0.022820473 -4.508956 ;
	setAttr ".pt[196]" -type "float3" 1.2199287 -0.014099121 -5.2106562 ;
	setAttr ".pt[197]" -type "float3" 1.2820435 -0.028141975 -4.5034075 ;
	setAttr ".pt[198]" -type "float3" 1.338047 -0.021359444 -5.2731047 ;
	setAttr ".pt[199]" -type "float3" 1.5009232 -0.042093277 -4.5831094 ;
	setAttr ".pt[200]" -type "float3" 1.5140038 -0.053560257 -3.8181331 ;
	setAttr ".pt[201]" -type "float3" 1.1923599 -0.032835007 -3.7172449 ;
	setAttr ".pt[202]" -type "float3" -1.0561829 0.11711884 -3.3785846 ;
	setAttr ".pt[203]" -type "float3" -1.1623955 0.12338066 -3.303107 ;
	setAttr ".pt[204]" -type "float3" -0.97033882 0.12116051 -4.0985146 ;
	setAttr ".pt[205]" -type "float3" -0.9693203 0.10862637 -3.1962938 ;
	setAttr ".pt[206]" -type "float3" -0.79674911 0.10768795 -3.9874063 ;
	setAttr ".pt[207]" -type "float3" 0.8779583 -0.0079269409 -3.9551954 ;
	setAttr ".pt[208]" -type "float3" 0.031667709 0.042264938 -3.3755279 ;
	setAttr ".pt[209]" -type "float3" 0.77204323 -0.010112762 -3.2696776 ;
	setAttr ".pt[210]" -type "float3" -0.63812256 0.088699341 -3.4025047 ;
	setAttr ".pt[211]" -type "float3" 1.2207813 -0.041072845 -3.2623849 ;
	setAttr ".pt[212]" -type "float3" 1.4194794 -0.057790756 -3.0413744 ;
	setAttr ".pt[213]" -type "float3" 1.4154682 -0.055812836 -3.1646409 ;
	setAttr ".pt[214]" -type "float3" 1.064045 -0.034583092 -2.9519885 ;
	setAttr ".pt[215]" -type "float3" -0.28389549 0.070456505 -3.8453465 ;
	setAttr ".pt[216]" -type "float3" -0.37983513 0.066081047 -3.05107 ;
	setAttr ".pt[217]" -type "float3" 0.4814167 0.016308784 -3.735538 ;
	setAttr ".pt[218]" -type "float3" 0.40364838 0.010849953 -2.9532659 ;
	setAttr ".pt[219]" -type "float3" -0.79425621 -0.041124344 -1.8476244 ;
	setAttr ".pt[220]" -type "float3" -1.0857124 -0.044214249 -2.4913924 ;
	setAttr ".pt[221]" -type "float3" -1.2415276 -0.056192398 -2.4243982 ;
	setAttr ".pt[222]" -type "float3" -0.92134857 -0.051959991 -1.8141918 ;
	setAttr ".pt[223]" -type "float3" -1.2213459 -0.056331635 -2.3101404 ;
	setAttr ".pt[224]" -type "float3" -0.88357162 -0.050020218 -1.7258655 ;
	setAttr ".pt[225]" -type "float3" -0.6185894 -0.0293293 -1.6472423 ;
	setAttr ".pt[226]" -type "float3" -0.89503098 -0.034546852 -2.2123649 ;
	setAttr ".pt[227]" -type "float3" -0.31790924 0.0055894852 -2.1719885 ;
	setAttr ".pt[228]" -type "float3" -0.18400192 0.0059099197 -1.5923092 ;
	setAttr ".pt[229]" -type "float3" 0.48257446 0.062438965 -2.2304406 ;
	setAttr ".pt[230]" -type "float3" 0.39221001 0.05384922 -1.5707417 ;
	setAttr ".pt[231]" -type "float3" 1.0110378 0.10727406 -1.6534023 ;
	setAttr ".pt[232]" -type "float3" 1.1611576 0.11125469 -2.3687556 ;
	setAttr ".pt[233]" -type "float3" 1.3681412 0.12727356 -2.4860759 ;
	setAttr ".pt[234]" -type "float3" 1.2077503 0.12540627 -1.7631102 ;
	setAttr ".pt[235]" -type "float3" 1.2009544 0.11708069 -2.5594888 ;
	setAttr ".pt[236]" -type "float3" 1.0338078 0.11137009 -1.8386836 ;
	setAttr ".pt[237]" -type "float3" 0.57860184 0.073280334 -1.8628802 ;
	setAttr ".pt[238]" -type "float3" 0.71113586 0.083465576 -2.5876613 ;
	setAttr ".pt[239]" -type "float3" 0.042856216 0.03635788 -2.5661578 ;
	setAttr ".pt[240]" -type "float3" 0.064165115 0.030334473 -1.8733109 ;
	setAttr ".pt[241]" -type "float3" -0.64944458 -0.013118744 -2.5150309 ;
	setAttr ".pt[242]" -type "float3" -0.42525291 -0.010389328 -1.8652804 ;
	setAttr ".pt[243]" -type "float3" -0.73586273 -0.046327591 -1.2990186 ;
	setAttr ".pt[244]" -type "float3" -0.77126122 -0.048288345 -1.3813186 ;
	setAttr ".pt[245]" -type "float3" -0.52217674 -0.029136658 -1.1986817 ;
	setAttr ".pt[246]" -type "float3" 0.37097931 0.045862198 -1.0121846 ;
	setAttr ".pt[247]" -type "float3" -0.14819145 0.0020074844 -1.1013197 ;
	setAttr ".pt[248]" -type "float3" 0.92765427 0.093902588 -0.99203014 ;
	setAttr ".pt[249]" -type "float3" 0.99256325 0.10222626 -1.1912866 ;
	setAttr ".pt[250]" -type "float3" 1.1094456 0.11102295 -1.0915313 ;
	setAttr ".pt[251]" -type "float3" 0.56773949 0.066703796 -1.2911303 ;
	setAttr ".pt[252]" -type "float3" -0.35588264 -0.012081146 -1.3930018 ;
	setAttr ".pt[253]" -type "float3" 0.094764709 0.026445389 -1.3496007 ;
	setAttr ".pt[254]" -type "float3" -0.66164207 -0.038414001 -1.4080535 ;
	setAttr ".pt[255]" -type "float3" -0.80805588 -0.058813095 -0.83830523 ;
	setAttr ".pt[256]" -type "float3" -0.85336494 -0.062446594 -0.86042213 ;
	setAttr ".pt[257]" -type "float3" -0.53276253 -0.043272018 -0.21965766 ;
	setAttr ".pt[258]" -type "float3" -0.29293442 -0.023920059 -0.11134768 ;
	setAttr ".pt[259]" -type "float3" -0.5773735 -0.040273666 -0.72851241 ;
	setAttr ".pt[260]" -type "float3" -0.49153709 -0.040075302 -0.19139636 ;
	setAttr ".pt[261]" -type "float3" -0.18312836 -0.0077476501 -0.6033355 ;
	setAttr ".pt[262]" -type "float3" 0.3177166 0.034273148 -0.49621201 ;
	setAttr ".pt[263]" -type "float3" 0.20289421 0.017608643 5.6505203e-05 ;
	setAttr ".pt[264]" -type "float3" -0.18637466 -0.014865875 -0.096937314 ;
	setAttr ".pt[265]" -type "float3" 0.75374794 0.071796417 -0.47250938 ;
	setAttr ".pt[266]" -type "float3" 0.48163414 0.041223526 0.04271245 ;
	setAttr ".pt[267]" -type "float3" 0.89448547 0.084653854 -0.52023268 ;
	setAttr ".pt[268]" -type "float3" 0.83651924 0.080755234 -0.60408878 ;
	setAttr ".pt[269]" -type "float3" 0.49041176 0.043296814 -0.054369926 ;
	setAttr ".pt[270]" -type "float3" 0.65014267 0.056566238 -0.010409355 ;
	setAttr ".pt[271]" -type "float3" 0.44619179 0.046722412 -0.59246731 ;
	setAttr ".pt[272]" -type "float3" 0.18303108 0.016702652 -0.060630322 ;
	setAttr ".pt[273]" -type "float3" 0.083938599 0.016410828 -0.67599368 ;
	setAttr ".pt[274]" -type "float3" -0.29744911 -0.015769958 -0.74406374 ;
	setAttr ".pt[275]" -type "float3" -0.051620483 -0.0037136078 -0.056813002 ;
	setAttr ".pt[276]" -type "float3" 0.013912201 0.001657486 -0.033357382 ;
	setAttr ".pt[277]" -type "float3" -0.66370773 -0.046302795 -0.83694959 ;
	setAttr ".pt[278]" -type "float3" -0.10458565 -0.0083732605 -0.052077055 ;
	setAttr ".pt[279]" -type "float3" -1.5563431 -0.037382126 -5.2000036 ;
	setAttr ".pt[280]" -type "float3" -1.5094643 -0.030605316 -5.457159 ;
	setAttr ".pt[281]" -type "float3" -1.4609108 -0.029743195 -5.2778373 ;
	setAttr ".pt[282]" -type "float3" -1.4752846 -0.035568237 -4.9277768 ;
	setAttr ".pt[283]" -type "float3" -1.1773453 -0.017375946 -4.7614594 ;
	setAttr ".pt[284]" -type "float3" -1.3608437 -0.02604866 -5.0472031 ;
	setAttr ".pt[285]" -type "float3" -0.73761749 0.015558243 -4.9565687 ;
	setAttr ".pt[286]" -type "float3" -0.93827057 0.0052852631 -5.2117639 ;
	setAttr ".pt[287]" -type "float3" -0.49879456 0.038011551 -5.3929663 ;
	setAttr ".pt[288]" -type "float3" -0.72504425 0.025661469 -5.6252851 ;
	setAttr ".pt[289]" -type "float3" -0.70010757 0.027526855 -5.6360927 ;
	setAttr ".pt[290]" -type "float3" -0.97898102 0.0082454681 -5.6286492 ;
	setAttr ".pt[291]" -type "float3" -1.10429 0.00094413757 -5.7239408 ;
	setAttr ".pt[292]" -type "float3" -0.89891052 0.0154953 -5.7548294 ;
	setAttr ".pt[293]" -type "float3" -1.2098427 -0.0091552734 -5.5183868 ;
	setAttr ".pt[294]" -type "float3" -1.3127365 -0.01471138 -5.6284132 ;
	setAttr ".pt[295]" -type "float3" -1.0851555 0.0024394989 -5.736896 ;
	setAttr ".pt[296]" -type "float3" -1.1444664 -0.0051727295 -5.4811831 ;
	setAttr ".pt[297]" -type "float3" -1.5449142 -0.033607483 -5.4163032 ;
	setAttr ".pt[298]" -type "float3" -1.4727859 -0.026327133 -5.5843 ;
	setAttr ".pt[299]" -type "float3" -1.4569626 -0.02728653 -5.4360151 ;
	setAttr ".pt[300]" -type "float3" -1.3464355 -0.016054153 -5.6988621 ;
	setAttr ".pt[301]" -type "float3" -1.3510742 -0.016702652 -5.6750879 ;
	setAttr ".pt[302]" -type "float3" -1.2292595 -0.0073032379 -5.7490335 ;
	setAttr ".pt[303]" -type "float3" -1.0589104 0.0048999786 -5.7844286 ;
	setAttr ".pt[304]" -type "float3" -1.3862572 -0.027357101 -5.0790443 ;
	setAttr ".pt[305]" -type "float3" -1.0808487 -0.00071144104 -5.4873757 ;
	setAttr ".pt[306]" -type "float3" -1.383255 -0.033540726 -4.6163707 ;
	setAttr ".pt[307]" -type "float3" -1.0256424 -0.01156044 -4.4273181 ;
	setAttr ".pt[308]" -type "float3" -0.54353714 0.025211334 -4.6891084 ;
	setAttr ".pt[309]" -type "float3" -0.33072281 0.046806335 -5.1929803 ;
	setAttr ".pt[310]" -type "float3" -0.49236298 0.041151047 -5.5882664 ;
	setAttr ".pt[311]" -type "float3" -0.85668945 0.01673317 -5.6343355 ;
	setAttr ".pt[312]" -type "float3" -1.0196381 0.015378952 -6.3474131 ;
	setAttr ".pt[313]" -type "float3" -1.1277313 0.013341904 -6.7380962 ;
	setAttr ".pt[314]" -type "float3" -0.97787094 0.023893356 -6.755837 ;
	setAttr ".pt[315]" -type "float3" -0.88815308 0.024652481 -6.3641853 ;
	setAttr ".pt[316]" -type "float3" -0.62625885 0.050327301 -6.9189725 ;
	setAttr ".pt[317]" -type "float3" -1.0061913 0.022989273 -6.8312817 ;
	setAttr ".pt[318]" -type "float3" -0.95503998 0.020271301 -6.3799257 ;
	setAttr ".pt[319]" -type "float3" -0.50051117 0.052959442 -6.48352 ;
	setAttr ".pt[320]" -type "float3" -0.078987122 0.082756042 -6.5420351 ;
	setAttr ".pt[321]" -type "float3" -0.22268677 0.078952789 -6.9820662 ;
	setAttr ".pt[322]" -type "float3" -0.23330688 0.071176529 -6.4720311 ;
	setAttr ".pt[323]" -type "float3" -0.30571747 0.071882248 -6.8835106 ;
	setAttr ".pt[324]" -type "float3" -0.1181221 0.085613251 -6.943635 ;
	setAttr ".pt[325]" -type "float3" -0.010520935 0.087202072 -6.5229759 ;
	setAttr ".pt[326]" -type "float3" -0.59778976 0.047204971 -6.551208 ;
	setAttr ".pt[327]" -type "float3" -0.64142227 0.04813385 -6.8356237 ;
	setAttr ".pt[328]" -type "float3" -1.155426 0.017255783 -7.1591506 ;
	setAttr ".pt[329]" -type "float3" -0.97132492 0.033353806 -7.4080777 ;
	setAttr ".pt[330]" -type "float3" -0.94607925 0.034627914 -7.3745332 ;
	setAttr ".pt[331]" -type "float3" -1.0106392 0.026874542 -7.1347375 ;
	setAttr ".pt[332]" -type "float3" -0.51176453 0.065092087 -7.4177179 ;
	setAttr ".pt[333]" -type "float3" -0.30516815 0.077138901 -7.2612729 ;
	setAttr ".pt[334]" -type "float3" -0.43116379 0.067512512 -7.1916895 ;
	setAttr ".pt[335]" -type "float3" -0.53628922 0.062587738 -7.3584919 ;
	setAttr ".pt[336]" -type "float3" -0.71285248 0.050941467 -7.3944235 ;
	setAttr ".pt[337]" -type "float3" -0.73244476 0.050519943 -7.4614573 ;
	setAttr ".pt[338]" -type "float3" -0.72512817 0.050258636 -7.4060688 ;
	setAttr ".pt[339]" -type "float3" -0.92058563 0.036237717 -7.3642297 ;
	setAttr ".pt[340]" -type "float3" -0.99764252 0.028579712 -7.1935 ;
	setAttr ".pt[341]" -type "float3" -0.69594193 0.050020218 -7.2435789 ;
	setAttr ".pt[342]" -type "float3" -0.40463257 0.070665359 -7.2878494 ;
	setAttr ".pt[343]" -type "float3" -0.53866196 0.063100815 -7.407527 ;
	setAttr ".pt[344]" -type "float3" -0.67563248 0.050310135 -7.1635103 ;
	setAttr ".pt[345]" -type "float3" -1.2009277 0.013145447 -7.0882015 ;
	setAttr ".pt[346]" -type "float3" -1.246685 0.0046405792 -6.7003565 ;
	setAttr ".pt[347]" -type "float3" -1.0209503 0.020675659 -6.7373166 ;
	setAttr ".pt[348]" -type "float3" -1.0163269 0.025898933 -7.0924397 ;
	setAttr ".pt[349]" -type "float3" -0.68069839 0.045084 -6.8103657 ;
	setAttr ".pt[350]" -type "float3" -0.72244644 0.046320915 -7.1077461 ;
	setAttr ".pt[351]" -type "float3" -0.41863632 0.062912941 -6.7964382 ;
	setAttr ".pt[352]" -type "float3" -0.52600479 0.059300423 -7.0694332 ;
	setAttr ".pt[353]" -type "float3" -0.32324982 0.069320679 -6.7853904 ;
	setAttr ".pt[354]" -type "float3" -0.48593521 0.06188488 -7.0569735 ;
	setAttr ".pt[355]" -type "float3" -0.43157959 0.0617342 -6.775538 ;
	setAttr ".pt[356]" -type "float3" -0.74856186 0.03963089 -6.7534852 ;
	setAttr ".pt[357]" -type "float3" -0.77296829 0.042609215 -7.0906067 ;
	setAttr ".pt[358]" -type "float3" -0.55458832 0.057217598 -7.0609288 ;
	setAttr ".pt[359]" -type "float3" -1.1969147 0.0082845688 -6.7163868 ;
	setAttr ".pt[360]" -type "float3" -1.1308746 0.018621445 -7.1358633 ;
	setAttr ".pt[361]" -type "float3" -1.0086288 0.029776573 -7.334795 ;
	setAttr ".pt[362]" -type "float3" -0.95388412 0.032758713 -7.2781038 ;
	setAttr ".pt[363]" -type "float3" -0.74946976 0.047182083 -7.3046055 ;
	setAttr ".pt[364]" -type "float3" -0.78319168 0.045395851 -7.343184 ;
	setAttr ".pt[365]" -type "float3" -0.62285995 0.054862976 -7.2303133 ;
	setAttr ".pt[366]" -type "float3" -0.61430359 0.055288315 -7.2185287 ;
	setAttr ".pt[367]" -type "float3" -0.99148941 0.030772209 -7.3215523 ;
	setAttr ".pt[368]" -type "float3" -0.78491974 0.044534683 -7.2894616 ;
	setAttr ".pt[369]" -type "float3" -0.63799667 0.053565979 -7.2117844 ;
	setAttr ".pt[370]" -type "float3" -1.171463 0.0039491653 -6.2758217 ;
	setAttr ".pt[371]" -type "float3" -1.166153 0.0043783188 -6.2804704 ;
	setAttr ".pt[372]" -type "float3" -0.89487457 0.023628235 -6.3234558 ;
	setAttr ".pt[373]" -type "float3" -0.63071442 0.044009209 -6.4837832 ;
	setAttr ".pt[374]" -type "float3" -0.29279709 0.066418648 -6.4237461 ;
	setAttr ".pt[375]" -type "float3" -0.1436615 0.076726913 -6.4275198 ;
	setAttr ".pt[376]" -type "float3" -0.28796387 0.066227913 -6.3859215 ;
	setAttr ".pt[377]" -type "float3" -0.68302536 0.03834343 -6.3340812 ;
	setAttr ".pt[378]" -type "float3" -0.91139221 0.016572952 -5.8949704 ;
	setAttr ".pt[379]" -type "float3" -0.38100815 0.053806305 -5.9499025 ;
	setAttr ".pt[380]" -type "float3" 0.039241791 0.084005356 -6.043992 ;
	setAttr ".pt[381]" -type "float3" -1.3554115 -0.038694382 -4.104732 ;
	setAttr ".pt[382]" -type "float3" -1.4100533 -0.032283783 -4.8408155 ;
	setAttr ".pt[383]" -type "float3" -1.086216 0.0044612885 -5.8885307 ;
	setAttr ".pt[384]" -type "float3" -1.0959053 0.0038814545 -5.8947978 ;
	setAttr ".pt[385]" -type "float3" -1.220192 -0.01117897 -5.423419 ;
	setAttr ".pt[386]" -type "float3" -1.1974182 -0.0096855164 -5.4181066 ;
	setAttr ".pt[387]" -type "float3" -1.3842735 -0.032632828 -4.6872053 ;
	setAttr ".pt[388]" -type "float3" -1.4694042 -0.048877716 -3.9350593 ;
	setAttr ".pt[389]" -type "float3" -1.1266136 0.0014133453 -5.8689814 ;
	setAttr ".pt[390]" -type "float3" -1.1576958 -0.0017738342 -5.7930679 ;
	setAttr ".pt[391]" -type "float3" -1.1362343 -0.00029182434 -5.7934766 ;
	setAttr ".pt[392]" -type "float3" -0.65802765 0.033133507 -5.8325028 ;
	setAttr ".pt[393]" -type "float3" -0.11262512 0.071838379 -5.9192038 ;
	setAttr ".pt[394]" -type "float3" 0.1032753 0.087507248 -5.9786201 ;
	setAttr ".pt[395]" -type "float3" 0.1255188 0.089888573 -6.0403576 ;
	setAttr ".pt[396]" -type "float3" 0.15350342 0.09188652 -6.0456319 ;
	setAttr ".pt[397]" -type "float3" 0.14497757 0.09145546 -6.0568423 ;
	setAttr ".pt[398]" -type "float3" -0.13498306 0.0651474 -5.5461259 ;
	setAttr ".pt[399]" -type "float3" 0.23058701 0.090251923 -5.5436077 ;
	setAttr ".pt[400]" -type "float3" -0.0080986023 0.066249847 -4.9943018 ;
	setAttr ".pt[401]" -type "float3" -0.15284729 0.045808792 -4.2352667 ;
	setAttr ".pt[402]" -type "float3" 0.41621017 0.10296822 -5.5400038 ;
	setAttr ".pt[403]" -type "float3" 0.4097023 0.10343933 -5.6065049 ;
	setAttr ".pt[404]" -type "float3" 0.4251709 0.095178604 -4.9315314 ;
	setAttr ".pt[405]" -type "float3" 0.52419662 0.091854095 -4.1980591 ;
	setAttr ".pt[406]" -type "float3" 0.69281006 0.11314964 -4.900126 ;
	setAttr ".pt[407]" -type "float3" 0.88817787 0.11640549 -4.1633329 ;
	setAttr ".pt[408]" -type "float3" 0.75326538 0.11686325 -4.8679395 ;
	setAttr ".pt[409]" -type "float3" 0.38007736 0.099286079 -5.4533906 ;
	setAttr ".pt[410]" -type "float3" 0.64025116 0.10765457 -4.7639894 ;
	setAttr ".pt[411]" -type "float3" 0.060424805 0.075680733 -5.3359108 ;
	setAttr ".pt[412]" -type "float3" 0.18225098 0.074207306 -4.6228318 ;
	setAttr ".pt[413]" -type "float3" -0.61131668 0.027989388 -5.2276173 ;
	setAttr ".pt[414]" -type "float3" -0.542099 0.022820473 -4.508956 ;
	setAttr ".pt[415]" -type "float3" -1.2199287 -0.014099121 -5.2106562 ;
	setAttr ".pt[416]" -type "float3" -1.2820435 -0.028141975 -4.5034075 ;
	setAttr ".pt[417]" -type "float3" -1.338047 -0.021359444 -5.2731047 ;
	setAttr ".pt[418]" -type "float3" -1.5009232 -0.042093277 -4.5831094 ;
	setAttr ".pt[419]" -type "float3" -1.5140038 -0.053560257 -3.8181331 ;
	setAttr ".pt[420]" -type "float3" -1.1923599 -0.032835007 -3.7172449 ;
	setAttr ".pt[421]" -type "float3" 1.0561829 0.11711884 -3.3785846 ;
	setAttr ".pt[422]" -type "float3" 1.1623955 0.12338066 -3.303107 ;
	setAttr ".pt[423]" -type "float3" 0.97033882 0.12116051 -4.0985146 ;
	setAttr ".pt[424]" -type "float3" 0.9693203 0.10862637 -3.1962938 ;
	setAttr ".pt[425]" -type "float3" 0.79674911 0.10768795 -3.9874063 ;
	setAttr ".pt[426]" -type "float3" -0.8779583 -0.0079269409 -3.9551954 ;
	setAttr ".pt[427]" -type "float3" -0.77204323 -0.010112762 -3.2696776 ;
	setAttr ".pt[428]" -type "float3" -0.031667709 0.042264938 -3.3755279 ;
	setAttr ".pt[429]" -type "float3" 0.63812256 0.088699341 -3.4025047 ;
	setAttr ".pt[430]" -type "float3" -1.2207813 -0.041072845 -3.2623849 ;
	setAttr ".pt[431]" -type "float3" -1.4194794 -0.057790756 -3.0413744 ;
	setAttr ".pt[432]" -type "float3" -1.4154682 -0.055812836 -3.1646409 ;
	setAttr ".pt[433]" -type "float3" -1.064045 -0.034583092 -2.9519885 ;
	setAttr ".pt[434]" -type "float3" 0.28389549 0.070456505 -3.8453465 ;
	setAttr ".pt[435]" -type "float3" 0.37983513 0.066081047 -3.05107 ;
	setAttr ".pt[436]" -type "float3" -0.4814167 0.016308784 -3.735538 ;
	setAttr ".pt[437]" -type "float3" -0.40364838 0.010849953 -2.9532659 ;
	setAttr ".pt[648]" -type "float3" -0.23736954 -0.020013809 -0.043534279 ;
	setAttr ".pt[649]" -type "float3" 0.23736954 -0.020013809 -0.043534279 ;
	setAttr ".pt[1169]" -type "float3" -0.18118668 0.015245438 0.035496235 ;
	setAttr ".pt[1170]" -type "float3" 0.18118668 0.015245438 0.035496235 ;
	setAttr ".pt[1171]" -type "float3" -0.16055965 0.013406754 0.039048672 ;
	setAttr ".pt[1172]" -type "float3" 0.16055965 0.013406754 0.039048672 ;
	setAttr ".pt[1173]" -type "float3" -0.16222382 0.013505936 0.042517662 ;
	setAttr ".pt[1174]" -type "float3" 0.16222382 0.013505936 0.042517662 ;
	setAttr ".pt[1175]" -type "float3" -0.063910484 0.0052862167 0.019289255 ;
	setAttr ".pt[1176]" -type "float3" 0.063910484 0.0052862167 0.019289255 ;
	setAttr ".pt[1177]" -type "float3" 0.053049088 -0.0045385361 -0.0048294067 ;
	setAttr ".pt[1178]" -type "float3" -0.053049088 -0.0045385361 -0.0048294067 ;
	setAttr ".pt[1179]" -type "float3" 0.14236069 -0.012044907 -0.02306366 ;
	setAttr ".pt[1180]" -type "float3" -0.14236069 -0.012044907 -0.02306366 ;
	setAttr ".pt[1304]" -type "float3" -4.3042953e-16 0 0 ;
	setAttr ".pt[1306]" -type "float3" -2.9963724e-16 0 0 ;
	setAttr -s 1689 ".vt";
	setAttr ".vt[0:165]"  24.85073853 23.097709656 1.76203346 24.47406387 21.70171928 3.25450301
		 26.57780647 21.46217918 3.937397 26.99660492 23.17390251 2.53903151 26.13658333 19.42662048 4.39238548
		 24.11802673 19.82760239 3.72758698 23.60971832 17.84276199 3.49966741 25.61946869 17.28107452 4.23847437
		 23.26866722 16.088096619 2.35460019 25.32405663 15.31497192 3.081950665 25.39586067 14.065113068 1.14852881
		 23.15785789 14.97945213 0.51721048 23.3039856 14.5152626 -1.9153986 25.97500038 13.85922146 -1.42734778
		 24.064510345 15.84453583 -4.48522186 26.8754673 15.5396719 -3.49953961 27.53647804 18.11433792 -4.071907043
		 24.8714695 18.74586296 -5.22818995 25.32694244 21.34201813 -4.39159775 27.87452888 20.60846901 -3.49175692
		 25.33181572 22.9155426 -2.42804193 27.86266136 22.52173233 -1.87902701 27.5141468 23.24180222 0.29411763
		 25.15040016 23.32678795 -0.25856075 22.79162788 23.34206963 1.31607246 22.58713341 21.99651146 2.65223074
		 22.25795937 20.19720268 3.097411156 21.75835991 18.4132843 2.87625289 21.28959656 16.86544991 1.86394274
		 20.95946503 15.83348274 0.15400594 20.79561806 15.45711708 -2.18246555 21.10613632 16.8269043 -4.62606955
		 21.86538124 19.46861458 -5.3413949 22.4605217 21.94021225 -4.74329758 22.81337357 23.40306091 -2.80237794
		 22.85768509 23.6907177 -0.68591177 20.80477142 24.13505554 1.16110659 20.78178024 22.78424263 3.027774334
		 20.40101814 20.67815208 3.61463642 19.79150009 18.69853592 3.17833281 19.22773361 17.29722977 1.87777376
		 18.80651093 16.51014137 0.067991406 18.59328079 16.58434105 -2.19067144 18.83982086 18.09737587 -4.10271597
		 19.30545807 20.22290611 -4.67006731 19.817173 22.19843292 -4.34613466 20.29939842 23.65409851 -2.96841717
		 20.58491135 24.1943531 -0.97518253 18.54572868 24.77495193 0.88198948 18.42152214 23.76784515 3.32865238
		 17.98699379 21.2361908 4.26706886 17.70584488 18.96184731 3.39208412 17.11893082 17.69931221 1.91800046
		 16.73216438 17.21145821 0.093112998 16.45519638 17.48330498 -2.16866374 16.49850082 18.69348717 -3.70689678
		 17.070274353 20.88230133 -4.42875433 17.54868507 22.68803787 -4.34090328 18.018878937 24.36055946 -3.3031106
		 18.31593513 25.090871811 -1.30994582 36.12652206 22.57756615 5.73815012 35.25115585 22.75858498 6.41511583
		 34.58006287 21.60793877 6.063488483 36.00017166138 21.2258358 5.29563618 34.19618988 23.64124107 6.29773712
		 33.89254379 24.70731735 5.28616571 32.85005569 23.82526398 4.62667036 33.18190002 22.55832291 5.83572674
		 34.75036621 24.98528671 3.9956224 33.92995071 23.95262718 3.23324513 35.59888077 23.4660511 2.80484605
		 36.21460342 24.4390564 3.45033455 36.81455231 23.43911362 4.19535732 36.69283295 22.35369301 3.49882984
		 36.61153793 22.86693001 5.06649971 36.61647415 21.78355789 4.66727066 36.89100266 25.23056793 4.29363441
		 37.075500488 25.40500832 5.49178743 37.18631363 24.63607025 5.42186165 37.1411438 24.17330742 4.89418507
		 36.65354538 24.057701111 6.28403378 36.32980347 24.41941071 6.628901 35.85877609 23.69211578 6.67138052
		 36.48229599 23.43654251 6.13075495 36.794384 24.97712708 6.29394388 36.96852493 24.34327316 5.93935585
		 35.072704315 24.44459152 6.70157003 35.78916931 24.8200798 6.77875042 35.95222855 25.64105415 6.52153301
		 34.75705338 25.46203041 6.020353317 36.28513718 26.11948013 5.56153584 35.52187347 25.9195385 4.77132702
		 36.90555191 23.67003441 5.52191734 41.30469513 19.19560242 5.8574295 40.19194412 19.75524902 5.71107912
		 39.74575043 18.4455452 6.031122208 41.051979065 18.25499344 6.32671833 40.68955231 20.1780529 4.55810404
		 41.63228607 19.44940376 4.93094444 41.89185333 19.057451248 4.025539875 41.050033569 19.56671906 3.30370688
		 41.79034805 18.078536987 3.68386626 40.89033127 18.25011063 2.93529224 41.21292496 16.88971901 4.79125261
		 40.18753052 16.8265152 4.51811409 40.53207016 17.17929268 3.48426318 41.45190048 17.28139496 4.036897182
		 40.96577835 17.067937851 5.83978796 39.79250717 17.21383858 5.5485878 41.91910172 18.057569504 5.8657074
		 41.85023499 18.70444107 5.69209623 42.1783371 18.026201248 4.41536093 42.17400742 18.62895012 4.50373411
		 42.20521164 18.015155792 5.13564873 42.083576202 18.74327087 5.094848156 41.77342987 17.29944229 5.75854683
		 41.95131302 17.26650047 5.029752731 41.93933487 17.45696449 4.45550585 38.81533051 20.14616966 5.32884359
		 38.58160782 18.71643257 5.49731016 38.6047554 17.32026863 5.070192814 39.30926895 16.83239365 4.23784351
		 39.26016235 17.10141373 3.048200607 39.62156296 18.38345528 2.38615394 39.75616837 19.96336746 2.63424087
		 39.41684341 20.80265045 3.95090365 37.88430786 12.98226643 5.86288881 39.26766586 12.99855804 6.16401005
		 39.29347229 14.47547054 6.33393908 38.022228241 14.73494911 5.87261581 39.59782028 15.55029678 5.65271139
		 40.010391235 15.70056438 4.61425447 39.017040253 15.92803574 4.29584694 38.37076569 15.84224319 5.050688267
		 40.061840057 15.17561817 3.77047896 38.97903061 15.67120266 3.19260669 40.0073623657 14.19531822 3.4494648
		 39.8531189 13.19744968 3.77367854 38.68090057 13.042304039 3.12207198 38.97585297 14.36178875 2.70431232
		 39.5817337 12.58211803 4.75755358 38.27225876 12.37301826 4.33746099 41.40041733 14.16198635 5.89332438
		 40.53609467 14.35256672 6.38136339 40.63598251 13.098788261 6.1661272 41.32076263 13.48826981 5.82314205
		 41.1595459 13.61584854 4.65103245 41.25667953 14.064594269 4.61858606 41.53511429 14.087582588 5.18287802
		 41.31998825 13.44557953 5.15260553 40.70902634 13.4014492 4.30860233 40.78004456 14.1147356 4.099151134
		 40.66129303 12.93613625 5.0082964897 40.69789886 15.16175652 5.8103075 41.2926445 14.80522442 5.70056963
		 41.47413635 14.73083019 5.065716743 40.90722656 15.35406208 4.89033651 41.25720596 14.55235386 4.59234667
		 40.84656143 14.80098915 4.24171066 37.34754562 20.81317711 4.95736122 37.8291626 21.41271019 3.31418872
		 38.2562294 20.22063065 2.014480829 31.47773361 21.13211823 5.47201777 33.70277023 19.99386024 5.99737597
		 37.12257004 19.11535645 5.48409414 37.026309967 17.49784851 5.49555349;
	setAttr ".vt[166:331]" 35.59709167 19.40554619 5.60485077 35.45746231 17.29691315 5.65046453
		 33.085186005 17.89792061 5.80999279 30.7080822 18.58558083 5.7128334 36.85718155 16.45150185 5.56539106
		 36.49494934 14.90661907 5.60447645 36.37957001 13.053273201 5.51113892 36.68331146 12.17980766 4.0048251152
		 37.28347778 12.93075085 2.33209658 37.68180466 14.40999317 1.69880676 37.9949913 15.95897007 1.68067002
		 38.13210297 17.051591873 1.64241207 38.21029663 18.64250946 1.6317997 36.70232391 21.82136917 2.3391149
		 36.79450989 20.60730362 1.163872 35.067249298 22.5266552 1.67124748 32.62664413 23.043636322 1.75699866
		 36.74954605 18.76345444 0.54900986 36.81216049 16.65949249 0.57379824 34.99063873 21.13781738 0.24915487
		 32.75733948 21.71146202 -0.42319486 34.88935089 19.15588951 -0.64114147 32.68567657 19.60947037 -1.62177515
		 34.66494751 16.93315506 -0.87931168 36.16896057 14.57300854 0.56218529 34.12572861 14.70408726 -0.60434878
		 35.51140976 12.75570297 1.49018276 33.3178215 12.8012743 0.7485705 34.78658676 12.10921097 3.55349135
		 32.56095886 12.3302269 2.97802901 34.49858475 13.14816952 5.48591423 32.24795151 13.43948078 5.33287382
		 34.78802109 15.34442711 5.92139196 32.54973602 15.75692177 6.097605228 30.15494156 16.2517662 5.76328993
		 29.83771324 13.93456268 4.66266298 30.31668091 20.066995621 -2.53972077 29.96587563 17.66525269 -2.9468658
		 32.36159134 17.28116798 -1.9464395 29.36651611 15.27836037 -2.4220767 31.76103783 14.9323101 -1.48495615
		 31.37962151 23.1029129 3.91308737 29.96800995 23.084379196 0.94401497 29.25758743 23.036178589 3.23406792
		 30.32240868 22.10282898 -1.17627025 28.88244629 21.19498253 4.62546158 27.77614784 16.73573303 5.082308292
		 28.32586861 19.024372101 5.16287279 27.52377319 14.5851326 3.8827734 30.93142128 13.15607643 0.04289946
		 28.48899078 13.51093674 -0.65311056 30.17226791 12.76367855 2.40515256 27.77438164 13.34627438 1.77581668
		 -24.47406387 21.70171928 3.25450301 -26.57780647 21.46217918 3.937397 -26.13658333 19.42662048 4.39238548
		 -24.11802673 19.82760239 3.72758698 -25.61946869 17.28107452 4.23847437 -23.60971832 17.84276199 3.49966741
		 -23.26866722 16.088096619 2.35460019 -25.32405663 15.31497192 3.081950665 -25.39586067 14.065113068 1.14852881
		 -23.15785789 14.97945213 0.51721048 -25.97500038 13.85922146 -1.42734778 -23.3039856 14.5152626 -1.9153986
		 -24.064510345 15.84453583 -4.48522186 -26.8754673 15.5396719 -3.49953961 -27.53647804 18.11433792 -4.071907043
		 -24.8714695 18.74586296 -5.22818995 -27.87452888 20.60846901 -3.49175692 -25.32694244 21.34201813 -4.39159775
		 -25.33181572 22.9155426 -2.42804193 -27.86266136 22.52173233 -1.87902701 -27.5141468 23.24180222 0.29411763
		 -25.15040016 23.32678795 -0.25856075 -26.99660492 23.17390251 2.53903151 -24.85073853 23.097709656 1.76203346
		 -21.75835991 18.4132843 2.87625289 -22.25795937 20.19720268 3.097411156 -21.28959656 16.86544991 1.86394274
		 -20.79561806 15.45711708 -2.18246555 -20.95946503 15.83348274 0.15400594 -21.10613632 16.8269043 -4.62606955
		 -22.4605217 21.94021225 -4.74329758 -21.86538124 19.46861458 -5.3413949 -22.81337357 23.40306091 -2.80237794
		 -22.79162788 23.34206963 1.31607246 -22.85768509 23.6907177 -0.68591177 -22.58713341 21.99651146 2.65223074
		 -20.40101814 20.67815208 3.61463642 -19.79150009 18.69853592 3.17833281 -17.70584488 18.96184731 3.39208412
		 -17.98699379 21.2361908 4.26706886 -19.22773361 17.29722977 1.87777376 -17.11893082 17.69931221 1.91800046
		 -18.80651093 16.51014137 0.067991406 -18.59328079 16.58434105 -2.19067144 -16.45519638 17.48330498 -2.16866374
		 -16.73216438 17.21145821 0.093112998 -18.83982086 18.09737587 -4.10271597 -16.49850082 18.69348717 -3.70689678
		 -19.30545807 20.22290611 -4.67006731 -19.817173 22.19843292 -4.34613466 -17.54868507 22.68803787 -4.34090328
		 -17.070274353 20.88230133 -4.42875433 -20.29939842 23.65409851 -2.96841717 -18.018878937 24.36055946 -3.3031106
		 -20.58491135 24.1943531 -0.97518253 -20.80477142 24.13505554 1.16110659 -18.54572868 24.77495193 0.88198948
		 -18.31593513 25.090871811 -1.30994582 -20.78178024 22.78424263 3.027774334 -18.42152214 23.76784515 3.32865238
		 -35.072704315 24.44459152 6.70157003 -35.85877609 23.69211578 6.67138052 -35.25115585 22.75858498 6.41511583
		 -34.19618988 23.64124107 6.29773712 -33.89254379 24.70731735 5.28616571 -34.75705338 25.46203041 6.020353317
		 -34.75036621 24.98528671 3.9956224 -35.52187347 25.9195385 4.77132702 -36.21460342 24.4390564 3.45033455
		 -36.89100266 25.23056793 4.29363441 -36.81455231 23.43911362 4.19535732 -36.61153793 22.86693001 5.06649971
		 -36.90555191 23.67003441 5.52191734 -37.1411438 24.17330742 4.89418507 -36.12652206 22.57756615 5.73815012
		 -36.48229599 23.43654251 6.13075495 -37.075500488 25.40500832 5.49178743 -36.28513718 26.11948013 5.56153584
		 -35.78916931 24.8200798 6.77875042 -36.32980347 24.41941071 6.628901 -35.95222855 25.64105415 6.52153301
		 -36.794384 24.97712708 6.29394388 -36.65354538 24.057701111 6.28403378 -36.96852493 24.34327316 5.93935585
		 -37.18631363 24.63607025 5.42186165 -34.58006287 21.60793877 6.063488483 -36.00017166138 21.2258358 5.29563618
		 -33.18190002 22.55832291 5.83572674 -32.85005569 23.82526398 4.62667036 -33.92995071 23.95262718 3.23324513
		 -35.59888077 23.4660511 2.80484605 -36.69283295 22.35369301 3.49882984 -36.61647415 21.78355789 4.66727066
		 -38.58160782 18.71643257 5.49731016 -39.74575043 18.4455452 6.031122208 -39.79250717 17.21383858 5.5485878
		 -38.6047554 17.32026863 5.070192814 -40.68955231 20.1780529 4.55810404 -40.19194412 19.75524902 5.71107912
		 -38.81533051 20.14616966 5.32884359 -39.41684341 20.80265045 3.95090365 -39.75616837 19.96336746 2.63424087
		 -41.050033569 19.56671906 3.30370688 -39.26016235 17.10141373 3.048200607 -40.53207016 17.17929268 3.48426318
		 -40.89033127 18.25011063 2.93529224 -39.62156296 18.38345528 2.38615394 -39.30926895 16.83239365 4.23784351
		 -40.18753052 16.8265152 4.51811409 -41.051979065 18.25499344 6.32671833 -41.91910172 18.057569504 5.8657074
		 -41.77342987 17.29944229 5.75854683 -40.96577835 17.067937851 5.83978796;
	setAttr ".vt[332:497]" -42.1783371 18.026201248 4.41536093 -41.79034805 18.078536987 3.68386626
		 -41.45190048 17.28139496 4.036897182 -41.93933487 17.45696449 4.45550585 -41.95131302 17.26650047 5.029752731
		 -42.20521164 18.015155792 5.13564873 -42.083576202 18.74327087 5.094848156 -41.85023499 18.70444107 5.69209623
		 -41.30469513 19.19560242 5.8574295 -41.63228607 19.44940376 4.93094444 -41.89185333 19.057451248 4.025539875
		 -42.17400742 18.62895012 4.50373411 -41.21292496 16.88971901 4.79125261 -40.53609467 14.35256672 6.38136339
		 -39.29347229 14.47547054 6.33393908 -39.59782028 15.55029678 5.65271139 -40.69789886 15.16175652 5.8103075
		 -40.010391235 15.70056438 4.61425447 -40.90722656 15.35406208 4.89033651 -40.061840057 15.17561817 3.77047896
		 -40.84656143 14.80098915 4.24171066 -40.0073623657 14.19531822 3.4494648 -40.78004456 14.1147356 4.099151134
		 -39.8531189 13.19744968 3.77367854 -39.5817337 12.58211803 4.75755358 -40.66129303 12.93613625 5.0082964897
		 -40.70902634 13.4014492 4.30860233 -39.26766586 12.99855804 6.16401005 -40.63598251 13.098788261 6.1661272
		 -41.40041733 14.16198635 5.89332438 -41.2926445 14.80522442 5.70056963 -41.47413635 14.73083019 5.065716743
		 -41.53511429 14.087582588 5.18287802 -41.25667953 14.064594269 4.61858606 -41.25720596 14.55235386 4.59234667
		 -41.32076263 13.48826981 5.82314205 -41.31998825 13.44557953 5.15260553 -41.1595459 13.61584854 4.65103245
		 -37.88430786 12.98226643 5.86288881 -38.022228241 14.73494911 5.87261581 -38.37076569 15.84224319 5.050688267
		 -39.017040253 15.92803574 4.29584694 -38.97903061 15.67120266 3.19260669 -38.97585297 14.36178875 2.70431232
		 -38.68090057 13.042304039 3.12207198 -38.27225876 12.37301826 4.33746099 -37.34754562 20.81317711 4.95736122
		 -37.8291626 21.41271019 3.31418872 -38.2562294 20.22063065 2.014480829 -31.47773361 21.13211823 5.47201777
		 -33.70277023 19.99386024 5.99737597 -37.12257004 19.11535645 5.48409414 -37.026309967 17.49784851 5.49555349
		 -35.45746231 17.29691315 5.65046453 -35.59709167 19.40554619 5.60485077 -33.085186005 17.89792061 5.80999279
		 -30.7080822 18.58558083 5.7128334 -36.85718155 16.45150185 5.56539106 -36.49494934 14.90661907 5.60447645
		 -36.37957001 13.053273201 5.51113892 -36.68331146 12.17980766 4.0048251152 -37.28347778 12.93075085 2.33209658
		 -37.68180466 14.40999317 1.69880676 -37.9949913 15.95897007 1.68067002 -38.21029663 18.64250946 1.6317997
		 -38.13210297 17.051591873 1.64241207 -36.70232391 21.82136917 2.3391149 -36.79450989 20.60730362 1.163872
		 -35.067249298 22.5266552 1.67124748 -32.62664413 23.043636322 1.75699866 -36.74954605 18.76345444 0.54900986
		 -36.81216049 16.65949249 0.57379824 -34.99063873 21.13781738 0.24915487 -32.75733948 21.71146202 -0.42319486
		 -34.88935089 19.15588951 -0.64114147 -32.68567657 19.60947037 -1.62177515 -34.66494751 16.93315506 -0.87931168
		 -36.16896057 14.57300854 0.56218529 -34.12572861 14.70408726 -0.60434878 -35.51140976 12.75570297 1.49018276
		 -33.3178215 12.8012743 0.7485705 -34.78658676 12.10921097 3.55349135 -32.56095886 12.3302269 2.97802901
		 -34.49858475 13.14816952 5.48591423 -32.24795151 13.43948078 5.33287382 -34.78802109 15.34442711 5.92139196
		 -32.54973602 15.75692177 6.097605228 -30.15494156 16.2517662 5.76328993 -29.83771324 13.93456268 4.66266298
		 -30.31668091 20.066995621 -2.53972077 -29.96587563 17.66525269 -2.9468658 -32.36159134 17.28116798 -1.9464395
		 -29.36651611 15.27836037 -2.4220767 -31.76103783 14.9323101 -1.48495615 -31.37962151 23.1029129 3.91308737
		 -29.25758743 23.036178589 3.23406792 -29.96800995 23.084379196 0.94401497 -30.32240868 22.10282898 -1.17627025
		 -28.88244629 21.19498253 4.62546158 -27.77614784 16.73573303 5.082308292 -28.32586861 19.024372101 5.16287279
		 -27.52377319 14.5851326 3.8827734 -30.93142128 13.15607643 0.04289946 -28.48899078 13.51093674 -0.65311056
		 -30.17226791 12.76367855 2.40515256 -27.77438164 13.34627438 1.77581668 -2.34306026 34.96559525 20.91814232
		 -2.2196157 32.51942444 20.62088013 0 32.31592178 20.69094467 0 35.10572433 21.12889862
		 2.2196157 32.51942444 20.62088013 2.34306026 34.96559525 20.91814232 -4.90643311 34.79456329 19.95831108
		 -4.17585611 32.76998138 19.91769028 4.17585611 32.76998138 19.91769028 4.90643311 34.79456329 19.95831108
		 -7.39556885 32.99378204 18.94109726 -5.83190918 32.49903107 18.84064484 5.83190918 32.49903107 18.84064484
		 7.39556885 32.99378204 18.94109726 -7.66202641 30.59705353 19.074930191 -6.45758104 31.045326233 18.5755825
		 6.45758104 31.045326233 18.5755825 7.66202641 30.59705353 19.074930191 -7.03473711 28.56401634 19.47120667
		 -5.54106188 29.63932991 19.12098885 5.54106188 29.63932991 19.12098885 7.03473711 28.56401634 19.47120667
		 -5.46179008 26.78700066 20.12046432 -4.065557957 28.87125969 19.7692585 4.065557957 28.87125969 19.7692585
		 5.46179008 26.78700066 20.12046432 -2.92637062 25.78876495 20.89596367 -1.98411155 28.3021946 20.39482689
		 1.98411155 28.3021946 20.39482689 2.92637062 25.78876495 20.89596367 0 25.60197067 21.26198196
		 0 28.13377571 20.72650337 5.59928036 35.17763901 17.78707314 7.28076887 33.46931458 17.25005913
		 -7.28077078 33.46931076 17.25005722 -5.59928226 35.17763901 17.78707504 9.64212227 31.79985619 17.79554749
		 9.43236256 32.69417953 16.33788872 -9.4323597 32.69417953 16.33788872 -9.64212227 31.79985619 17.79554749
		 9.51857567 29.63160896 18.16875839 -9.51857567 29.63160896 18.16875839 8.69434834 27.20597649 18.50730133
		 -8.69434834 27.20597649 18.50730133 3.60436893 22.86522102 20.54005814 0 22.57315063 21.087688446
		 -3.60436893 22.86522102 20.54005814 7.28568888 23.95528603 19.060770035 -7.28568888 23.95528603 19.060770035
		 10.73800373 19.72241783 16.44986916 10.62619495 23.28033066 16.86578941 7.62880087 19.57051277 18.53390694
		 -7.62880087 19.57051277 18.53390694 -10.62619495 23.28033066 16.86578941 -10.73800373 19.72241783 16.44986916
		 3.98784113 19.012317657 20.1268959 0 18.75261879 20.81389046 -3.98784113 19.012317657 20.1268959
		 -11.52003288 29.012392044 16.6946888 11.52003288 29.012392044 16.6946888;
	setAttr ".vt[498:663]" -11.66648865 31.24106789 16.34375572 11.66648865 31.24106789 16.34375572
		 11.0064373016 26.4705677 16.86469841 -11.0064373016 26.4705677 16.86469841 11.20376587 32.31670761 15.23572826
		 -11.20376301 32.31671143 15.23572445 4.55604935 37.35525513 17.88216019 3.45102835 37.34038925 19.83683205
		 -3.45102835 37.34038925 19.83683205 -4.55604935 37.35525894 17.882164 1.94899142 37.46411896 20.41073418
		 -1.94899142 37.46411896 20.41073418 0 37.45892334 20.65005875 0 39.99711227 20.044509888
		 -1.83977139 39.94290924 19.81591415 1.83977139 39.94290924 19.81591415 -3.37272692 39.7562561 19.17947197
		 3.37272692 39.7562561 19.17947197 -4.41095018 39.56233978 17.59991646 4.41093445 39.56233978 17.5999012
		 0 42.52477646 19.162117 -1.98772597 42.43655396 18.94636536 1.98772597 42.43655396 18.94636536
		 -3.58765411 42.17740631 18.38972473 3.58765411 42.17740631 18.38972473 -4.70281696 41.48870468 17.20458031
		 4.70281315 41.48870468 17.20457077 0 45.074604034 18.072862625 -2.48104024 44.77361298 17.8674984
		 2.48104024 44.77361298 17.8674984 -4.29330206 44.27950668 17.45268631 4.29330206 44.27950668 17.45268631
		 -5.32283449 42.89931107 16.58550072 5.32283115 42.89931488 16.58549309 -5.30009365 45.65455627 16.46484375
		 -6.55205154 44.11572647 15.94740963 6.55205154 44.11572647 15.94740963 5.30009365 45.65455627 16.46484375
		 -7.58807898 46.91200256 15.23307514 -8.29198074 44.99228668 15.24681664 8.29198074 44.99228668 15.24681664
		 7.58807898 46.91200256 15.23307514 -10.41012859 47.021762848 13.48116589 -10.29585648 45.39748764 14.11555004
		 10.29585648 45.39748764 14.11555004 10.41012859 47.021762848 13.48116589 -15.3000679 43.94946671 10.80243587
		 -14.59858227 43.22073746 11.78678513 14.59858227 43.22073746 11.78678513 15.3000679 43.94946671 10.80243587
		 -16.69558334 41.84358978 10.010967255 -15.84148598 41.22957611 10.94482517 15.84148598 41.22957611 10.94482517
		 16.69558334 41.84358978 10.010967255 -17.51910973 38.85097885 9.7535181 -16.44912338 38.66639328 10.49284649
		 16.44913483 38.66640472 10.49284649 17.51910973 38.85097885 9.7535181 -17.34350586 35.27264786 10.21838284
		 -15.96342373 35.37635803 10.95463467 15.96343517 35.37635803 10.95463943 17.34350586 35.27264786 10.21838284
		 -16.17391014 31.99546623 11.21019363 -14.68734169 33.43888855 12.20065212 14.68735313 33.43889236 12.2006588
		 16.17391014 31.99546623 11.21019363 -13.77912903 31.12100792 14.14732647 -13.26987457 32.55953979 13.54469967
		 13.26988411 32.55954361 13.5447073 13.77912903 31.12100792 14.14732647 0 47.63901138 16.73535919
		 -2.97171092 47.61933899 16.23565102 2.97171092 47.61933899 16.23565102 0 54.71444702 9.18865299
		 -4.23015404 53.95973969 8.7458353 4.23015404 53.95973969 8.7458353 0 56.39635468 4.37283373
		 -4.60287857 55.78657913 4.073598862 4.60287857 55.78657913 4.073598862 0 57.073230743 -0.30558062
		 -4.85206604 56.3697319 -0.89950913 4.85206604 56.3697319 -0.89950913 0 56.43016052 -6.63445616
		 -4.90413857 55.66534424 -6.56397867 4.90413857 55.66534424 -6.56397867 0 54.19335556 -12.32389355
		 -4.8790946 53.54144287 -11.95386124 4.8790946 53.54144287 -11.95386124 0 50.48313141 -16.95181465
		 -4.85921192 50.13543701 -16.37403488 4.85921192 50.13543701 -16.37403488 0 45.8247757 -20.2424469
		 -4.86782742 45.72704697 -19.53763962 4.86782742 45.72704697 -19.53763962 -6.50470638 48.67982483 14.33397579
		 6.50470638 48.67982483 14.33397579 -10.016140938 48.93421936 11.34410191 10.016140938 48.93421936 11.34410191
		 -8.11518478 52.65076828 8.042410851 8.11518478 52.65076828 8.042410851 -11.28046322 50.50154114 7.20904303
		 11.28046322 50.50154114 7.20904303 -15.52657223 44.7305603 8.11732769 15.52657223 44.7305603 8.11732769
		 -16.038360596 45.44698334 4.89859152 16.038360596 45.44698334 4.89859152 -17.17842293 42.1276474 7.30971193
		 17.17842293 42.1276474 7.30971193 -17.63365364 42.59679413 4.038021088 17.63365364 42.59679413 4.038021088
		 -18.11996078 38.95026779 6.9200387 18.11996078 38.95026779 6.9200387 -18.68526649 39.22370529 3.23358083
		 18.68526649 39.22370529 3.23358083 -18.14645004 35.43515396 7.19845963 18.14645004 35.43515396 7.19845963
		 -18.67424393 35.6060257 4.031484604 18.67424393 35.6060257 4.031484604 -17.7789917 31.98583221 7.46719122
		 17.7789917 31.98583221 7.46719122 -18.41504288 32.12953949 3.80793166 18.41504288 32.12953949 3.80793166
		 -17.31709862 28.43948746 7.4159894 -15.86847019 28.6098938 11.26426888 15.86847019 28.6098938 11.26426888
		 17.31709862 28.43948746 7.4159894 -18.19086838 28.068162918 2.56039429 18.19086838 28.068162918 2.56039429
		 13.8242712 28.63830185 14.40345669 -13.8242712 28.63830185 14.40345669 -16.6772995 25.16822052 8.17058372
		 -15.31433105 25.52116585 11.51976871 15.31433105 25.52116585 11.51976871 16.6772995 25.16822052 8.17058372
		 -17.49044228 24.66787338 4.92271948 17.49044228 24.66787338 4.92271948 13.37754822 25.9169693 14.54215717
		 -13.37754822 25.9169693 14.54215717 -16.4003067 22.043582916 8.74878025 -15.047444344 22.52051163 11.710742
		 15.047444344 22.52051163 11.710742 16.4003067 22.043582916 8.74878025 -17.10495949 21.67413139 5.78538132
		 17.10495949 21.67413139 5.78538132 13.14428806 22.86608124 14.43780994 -13.14428806 22.86608124 14.43780994
		 -16.05947113 19.15048218 9.034537315 -14.81783295 19.75042725 11.79249191 14.81783295 19.75042725 11.79249191
		 16.05947113 19.15048218 9.034537315 -16.86205101 18.72614098 5.3318553 16.86205101 18.72614098 5.3318553
		 13.13905334 19.82371712 14.15501499 -13.13905334 19.82371712 14.15501499 12.046497345 51.55910492 2.90027237
		 -12.046497345 51.55910492 2.90027237 8.77876949 54.084182739 3.55992556 -8.77876949 54.084182739 3.55992556
		 12.56581116 52.027801514 -1.62791669 9.098324776 54.54212952 -1.28458869 -9.098324776 54.54212952 -1.28458869
		 -12.56581116 52.027801514 -1.62791669 12.53911591 51.46375275 -6.20397902 9.15746307 53.8678627 -6.41815233
		 -9.15746307 53.8678627 -6.41815233 -12.53911591 51.46375275 -6.20397902;
	setAttr ".vt[664:829]" 12.27334976 49.94383621 -10.1851511 9.07305336 51.98585129 -11.079713821
		 -9.07305336 51.98585129 -11.079713821 -12.27334976 49.94383621 -10.1851511 16.86815071 46.21908569 -2.49975204
		 -16.86815071 46.21908569 -2.49975204 16.69064522 45.75848007 -6.29927397 -16.69064522 45.75848007 -6.29927397
		 15.95729637 44.92375946 -9.85115814 -15.95729637 44.92375946 -9.85115814 17.97418022 43.33758163 -2.93954778
		 -17.97418022 43.33758163 -2.93954778 17.75088882 42.63417435 -6.48681831 -17.75088882 42.63417435 -6.48681831
		 16.96315575 41.36408997 -10.052353859 -16.96315575 41.36408997 -10.052353859 9.020856857 49.10109329 -14.96163654
		 -9.020856857 49.10109329 -14.96163654 12.08263588 47.7425766 -13.33046436 -12.08263588 47.7425766 -13.33046436
		 14.58929348 44.36501694 -12.89369869 -14.58929348 44.36501694 -12.89369869 15.30841541 40.80078125 -13.62555599
		 -15.30841541 40.80078125 -13.62555599 9.12992287 45.27306747 -17.79057693 -9.12992287 45.27306747 -17.79057693
		 12.41093254 44.66474915 -15.46109104 -12.41093254 44.66474915 -15.46109104 12.73990536 40.69935989 -16.76956749
		 -12.73990536 40.69935989 -16.76956749 0 40.86332321 -21.95590782 -4.84888554 40.85788345 -21.27030754
		 4.84888554 40.85788345 -21.27030754 9.21144676 40.79921722 -19.42304993 -9.21144676 40.79921722 -19.42304993
		 31.079435349 35.54600525 -0.61000079 28.17465782 35.45807266 -0.64277089 -28.17465782 35.45807266 -0.64277089
		 -31.079435349 35.54600525 -0.61000079 28.041397095 33.23717117 -0.69115138 30.86966515 33.098232269 -0.60663849
		 -30.86966515 33.098232269 -0.60663849 -28.041397095 33.23717117 -0.69115138 27.73260689 29.93701172 -3.66090512
		 30.48027802 29.59422112 -3.96884084 -30.48027802 29.59422112 -3.96884084 -27.73260689 29.93701172 -3.66090512
		 27.51391792 29.89026451 -5.97864103 30.31448174 29.75903702 -6.39133644 -30.31448174 29.75903702 -6.39133644
		 -27.51391792 29.89026451 -5.97864103 27.2856884 33.68035126 -9.48612785 30.26885796 33.86576462 -9.42360115
		 -30.26885796 33.86576462 -9.42360115 -27.2856884 33.68035126 -9.48612785 27.38260841 36.48454285 -9.15537548
		 30.37549591 36.45702362 -9.1135788 -30.37549591 36.45702362 -9.1135788 -27.38260841 36.48454285 -9.15537548
		 27.89822578 39.19953918 -5.18498659 30.90782356 39.46846008 -5.46601915 -30.90782356 39.46846008 -5.46601915
		 -27.89822578 39.19953918 -5.18498659 28.17572021 38.95947266 -2.98776054 31.27272034 39.30821609 -3.22864652
		 -31.27272034 39.30821609 -3.22864652 -28.17572021 38.95947266 -2.98776054 27.61859322 38.50538254 -7.5291605
		 30.64791107 38.53028488 -7.6946907 -27.61859322 38.50538254 -7.5291605 -30.64791107 38.53028488 -7.6946907
		 27.33682632 31.053077698 -8.3498621 30.25342369 31.30932808 -8.54108238 -27.33682632 31.053077698 -8.3498621
		 -30.25342369 31.30932808 -8.54108238 27.8821907 31.15049744 -1.61887622 30.64687157 30.77394104 -1.63260961
		 -27.8821907 31.15049744 -1.61887622 -30.64687157 30.77394104 -1.63260961 28.26498222 37.56723022 -1.27307403
		 31.29394531 37.83476257 -1.35091519 -28.26498222 37.56723022 -1.27307403 -31.29394531 37.83476257 -1.35091519
		 25.1773262 38.69286346 -2.63978791 25.32030678 37.30964661 -1.10045218 25.28468704 35.3718605 -0.58039957
		 25.22949409 33.33060455 -0.70594043 -25.28468704 35.3718605 -0.58039957 -25.22949409 33.33060455 -0.70594043
		 25.15833855 31.49053955 -1.55506253 -25.15833855 31.49053955 -1.55506253 25.034210205 30.24461555 -3.26012874
		 24.7856884 30.044668198 -5.4413228 -25.034210205 30.24461555 -3.26012874 -24.7856884 30.044668198 -5.4413228
		 24.4406662 31.056173325 -7.77028131 -24.4406662 31.056173325 -7.77028131 24.28012657 33.56892014 -8.93725586
		 24.29008675 36.38064957 -8.74702263 -24.28012657 33.56892014 -8.93725586 -24.29008675 36.38064957 -8.74702263
		 24.55276299 38.45015717 -7.12244463 -24.55276299 38.45015717 -7.12244463 24.89271736 38.97458649 -4.76210451
		 -24.89271736 38.97458649 -4.76210451 -25.1773262 38.69286346 -2.63978791 -25.32030678 37.30964661 -1.10045218
		 19.16099548 39.35641479 -1.24614239 19.24469948 37.97637177 1.19371498 19.25726509 35.65252304 1.83655131
		 19.3058815 32.79197693 1.24986899 -19.25726509 35.65252304 1.83655131 -19.3058815 32.79197693 1.24986899
		 19.39686775 30.79223633 0.049789727 -19.39686775 30.79223633 0.049789727 19.55616188 29.66285706 -1.7697432
		 19.44944572 29.53455544 -4.12980795 -19.55616188 29.66285706 -1.7697432 -19.44944572 29.53455544 -4.12980795
		 19.11936378 30.77223587 -6.3482976 -19.11936378 30.77223587 -6.3482976 18.79230309 33.33483887 -7.59481049
		 18.67299652 36.089160919 -7.4308567 -18.79230309 33.33483887 -7.59481049 -18.67299652 36.089160919 -7.4308567
		 18.93027878 38.093395233 -5.70932674 -18.93027878 38.093395233 -5.70932674 19.048706055 39.034870148 -3.49934745
		 -19.048706055 39.034870148 -3.49934745 -19.16099548 39.35641479 -1.24614239 -19.24469948 37.97637177 1.19371498
		 22.20851135 38.69517517 -2.050127029 22.42357635 37.54122162 -0.14985448 22.39893723 35.45191574 0.43153879
		 22.3827858 33.20221329 0.10987359 22.36075783 31.3939724 -0.97676295 22.33995628 30.26594925 -2.65599465
		 22.15430069 30.095638275 -4.78783274 21.80288506 31.16030884 -6.94763088 21.57531166 33.51780701 -8.00045204163
		 21.52306366 36.065822601 -7.84235668 21.69095802 38.098419189 -6.39517975 21.94820404 38.76451874 -4.14193535
		 -22.20851135 38.69517517 -2.050127029 -22.42357635 37.54122162 -0.14985448 -22.39893723 35.45191574 0.43153879
		 -22.3827858 33.20221329 0.10987359 -22.36075783 31.3939724 -0.97676295 -22.33995628 30.26594925 -2.65599465
		 -22.15430069 30.095638275 -4.78783274 -21.80288506 31.16030884 -6.94763088 -21.57531166 33.51780701 -8.00045204163
		 -21.52306366 36.065822601 -7.84235668 -21.69095802 38.098419189 -6.39517975 -21.94820404 38.76451874 -4.14193535
		 -2.088863134 32.46427917 19.62247467 0 32.17950439 19.87882614 2.088863134 32.46427917 19.62247467
		 -3.98830032 32.80358124 19.10196114 3.98830032 32.80358124 19.10196114 -5.81232929 32.44737244 17.92568398
		 5.81232929 32.44737244 17.92568398 -6.26799536 30.9413147 17.30510521 6.26799536 30.9413147 17.30510521
		 -5.18717289 29.44996452 18.053262711 5.18717289 29.44996452 18.053262711;
	setAttr ".vt[830:995]" -3.794312 28.58905792 18.55278778 3.794312 28.58905792 18.55278778
		 -1.53971338 27.84398651 19.18529129 1.53971338 27.84398651 19.18529129 0 27.7315464 19.27254295
		 41.21343231 39.40084457 -3.09285593 41.90061188 42.15471268 -2.48098183 40.11634827 42.65589905 -1.99869275
		 38.8302002 40.089996338 -2.056202412 -38.8302002 40.089996338 -2.056202412 -40.11634827 42.65589905 -1.99869275
		 -41.90061188 42.15471268 -2.48098183 -41.21343231 39.40084457 -3.09285593 37.94297409 41.013149261 -3.17363
		 39.44555283 43.20027542 -2.72675991 -39.44555283 43.20027542 -2.72675991 -37.94297409 41.013149261 -3.17363
		 38.20093155 41.3274765 -4.82003212 39.76138306 43.47502518 -3.91851568 -39.76138306 43.47502518 -3.91851568
		 -38.20093155 41.3274765 -4.82003212 41.77375793 43.021274567 -4.6061554 41.2207489 40.30455017 -5.46050978
		 -41.2207489 40.30455017 -5.46050978 -41.77375793 43.021274567 -4.6061554 41.75818253 39.66003418 -4.25398445
		 42.57321548 42.48200226 -3.4693327 -42.57321548 42.48200226 -3.4693327 -41.75818253 39.66003418 -4.25398445
		 41.73967743 43.56619263 -3.85287309 40.33892822 43.81420517 -3.51167202 -40.33892822 43.81420517 -3.51167202
		 -41.73967743 43.56619263 -3.85287309 40.56408691 43.2750206 -2.12584567 41.86018372 42.88955688 -2.51089406
		 -41.86018372 42.88955688 -2.51089406 -40.56408691 43.2750206 -2.12584567 40.37039185 43.82526779 -2.70529103
		 42.052867889 43.3782959 -3.045527458 -40.37039185 43.82526779 -2.70529103 -42.052867889 43.3782959 -3.045527458
		 39.64976501 41.11937714 -5.77488661 40.68772507 43.32490158 -4.54180717 41.017864227 43.79006577 -3.73509955
		 41.31415558 43.7822876 -2.81687975 41.235466 43.096458435 -2.22145844 41.049739838 42.27481842 -2.061095476
		 40.07926178 39.68343735 -2.19220495 -39.64976501 41.11937714 -5.77488661 -40.68772507 43.32490158 -4.54180717
		 -41.017864227 43.79006577 -3.73509955 -41.31415558 43.7822876 -2.81687975 -41.235466 43.096458435 -2.22145844
		 -41.049739838 42.27481842 -2.061095476 -40.07926178 39.68343735 -2.19220495 41.70186615 41.061103821 -2.69763851
		 40.67967606 41.2387085 -2.05072999 39.53555679 41.68598557 -2.027714252 38.7796936 42.18222046 -2.94996572
		 39.029788971 42.54658127 -4.46138525 40.21910095 42.28859711 -5.21574688 41.4958725 41.76507187 -5.1187005
		 42.31843185 41.16947556 -3.8638854 -41.70186615 41.061103821 -2.69763851 -40.67967606 41.2387085 -2.05072999
		 -39.53555679 41.68598557 -2.027714252 -38.7796936 42.18222046 -2.94996572 -39.029788971 42.54658127 -4.46138525
		 -40.21910095 42.28859711 -5.21574688 -41.4958725 41.76507187 -5.1187005 -42.31843185 41.16947556 -3.8638854
		 43.88184738 38.25244522 -4.72552013 44.062999725 35.58090591 -4.61108303 47.044509888 35.76063538 -4.35821915
		 46.82460403 37.99090195 -4.89440536 -46.82460403 37.99090195 -4.89440536 -47.044509888 35.76063538 -4.35821915
		 -44.062999725 35.58090591 -4.61108303 -43.88184738 38.25244522 -4.72552013 46.85023117 38.33552933 -5.81948328
		 43.84886169 39.0082397461 -5.96805811 -43.84886169 39.0082397461 -5.96805811 -46.85023117 38.33552933 -5.81948328
		 44.21631241 38.28881454 -7.33937597 46.96088409 38.093772888 -6.8073101 -46.96088409 38.093772888 -6.8073101
		 -44.21631241 38.28881454 -7.33937597 44.4510498 35.9057045 -7.096409321 46.92606354 36.10596848 -6.64051628
		 -46.92606354 36.10596848 -6.64051628 -44.4510498 35.9057045 -7.096409321 44.64110947 35.28672791 -5.77238894
		 47.040794373 35.54516602 -5.7590065 -47.040794373 35.54516602 -5.7590065 -44.64110947 35.28672791 -5.77238894
		 47.53447723 37.84927368 -5.049441338 48.025127411 36.27257919 -4.72792816 -48.025127411 36.27257919 -4.72792816
		 -47.53447723 37.84927368 -5.049441338 47.64023972 37.78165817 -6.48832083 47.6832962 36.3407135 -6.44535112
		 -47.6832962 36.3407135 -6.44535112 -47.64023972 37.78165817 -6.48832083 48.047954559 36.087333679 -5.72549582
		 47.71271896 37.98107147 -5.74620485 -48.047954559 36.087333679 -5.72549582 -47.71271896 37.98107147 -5.74620485
		 43.91624451 36.9495163 -4.40454054 46.89753723 36.8842392 -4.14766216 47.9809227 36.94777679 -4.69140577
		 48.32432938 37.035625458 -5.66813421 47.80569839 37.055812836 -6.59834576 46.97151947 37.061283112 -7.089639187
		 44.45189285 37.1050148 -7.57378101 -43.91624451 36.9495163 -4.40454054 -46.89753723 36.8842392 -4.14766216
		 -47.9809227 36.94777679 -4.69140577 -48.32432938 37.035625458 -5.66813421 -47.80569839 37.055812836 -6.59834576
		 -46.97151947 37.061283112 -7.089639187 -44.45189285 37.1050148 -7.57378101 45.41081238 38.14519119 -4.73113155
		 45.49135208 36.87310028 -4.072295189 45.60560608 35.65613937 -4.41474438 45.88292313 35.436306 -5.79237986
		 45.789711 35.97692871 -6.83910942 45.79568481 37.060661316 -7.41448402 45.79594421 38.29566193 -7.12389183
		 45.44456863 38.61462402 -5.82893133 -45.41081238 38.14519119 -4.73113155 -45.49135208 36.87310028 -4.072295189
		 -45.60560608 35.65613937 -4.41474438 -45.88292313 35.436306 -5.79237986 -45.789711 35.97692871 -6.83910942
		 -45.79568481 37.060661316 -7.41448402 -45.79594421 38.29566193 -7.12389183 -45.44456863 38.61462402 -5.82893133
		 47.10772705 32.039802551 -4.51102686 47.13218307 33.7604332 -4.66606569 44.10103989 33.81376266 -4.39903784
		 44.26808929 31.38355637 -4.28095436 -44.26808929 31.38355637 -4.28095436 -44.10103989 33.81376266 -4.39903784
		 -47.13218307 33.7604332 -4.66606569 -47.10772705 32.039802551 -4.51102686 44.60975266 34.35667419 -5.75840235
		 47.25221252 34.17459488 -5.50197506 -47.25221252 34.17459488 -5.50197506 -44.60975266 34.35667419 -5.75840235
		 44.47881317 33.92613602 -6.96491766 47.26935577 33.69562531 -6.44589138 -47.26935577 33.69562531 -6.44589138
		 -44.47881317 33.92613602 -6.96491766 44.77089691 31.072399139 -6.95351839 47.42235184 31.76459122 -6.37480402
		 -47.42235184 31.76459122 -6.37480402 -44.77089691 31.072399139 -6.95351839 44.445961 30.66754532 -5.55946922
		 47.29302216 31.49623489 -5.43428612 -47.29302216 31.49623489 -5.43428612 -44.445961 30.66754532 -5.55946922
		 47.73391724 32.3439064 -4.79627371 47.72754669 33.47252655 -4.878829 -47.72754669 33.47252655 -4.878829
		 -47.73391724 32.3439064 -4.79627371 47.92053604 33.43548203 -6.071365356;
	setAttr ".vt[996:1161]" 47.96583176 32.27505112 -5.9973135 -47.96583176 32.27505112 -5.9973135
		 -47.92053604 33.43548203 -6.071365356 47.94514847 33.55413818 -5.43812323 47.98778152 32.20357513 -5.37070227
		 -47.94514847 33.55413818 -5.43812323 -47.98778152 32.20357513 -5.37070227 44.73544312 32.4264183 -7.41504431
		 47.36038971 32.73090744 -6.73505592 48.0066795349 32.84700394 -6.10112476 48.11097336 32.90652847 -5.38997793
		 47.82089615 32.92747116 -4.76596594 47.11141586 32.92086411 -4.29762363 44.25486374 32.52891922 -4.075623989
		 -44.73544312 32.4264183 -7.41504431 -47.36038971 32.73090744 -6.73505592 -48.0066795349 32.84700394 -6.10112476
		 -48.11097336 32.90652847 -5.38997793 -47.82089615 32.92747116 -4.76596594 -47.11141586 32.92086411 -4.29762363
		 -44.25486374 32.52891922 -4.075623989 45.92025375 31.71302986 -4.36052084 45.8651886 32.80566788 -4.094369411
		 45.92644501 33.8641243 -4.54356146 46.26018524 34.28622055 -5.63321161 46.010478973 33.7206955 -6.72030449
		 46.11208344 32.55827713 -7.14010382 46.20203018 31.33422089 -6.72553015 46.036560059 31.055225372 -5.52497149
		 -45.92025375 31.71302986 -4.36052084 -45.8651886 32.80566788 -4.094369411 -45.92644501 33.8641243 -4.54356146
		 -46.26018524 34.28622055 -5.63321161 -46.010478973 33.7206955 -6.72030449 -46.11208344 32.55827713 -7.14010382
		 -46.20203018 31.33422089 -6.72553015 -46.036560059 31.055225372 -5.52497149 42.19215775 39.34034348 -5.65570354
		 42.28575134 38.73295975 -4.26446962 -42.28575134 38.73295975 -4.26446962 -42.19215775 39.34034348 -5.65570354
		 42.43604279 38.4766922 -7.41203976 -42.43604279 38.4766922 -7.41203976 39.53292847 37.72646332 -1.91624463
		 41.15684891 37.44445038 -2.8188622 -41.15684891 37.44445038 -2.8188622 -39.53292847 37.72646332 -1.91624463
		 37.24999237 38.34946442 -1.75802147 -37.24999237 38.34946442 -1.75802147 42.43205643 37.25623703 -3.65693974
		 -42.43205643 37.25623703 -3.65693974 42.53432465 35.6336441 -3.50586271 -42.53432465 35.6336441 -3.50586271
		 41.15819931 35.067043304 -2.79182911 -41.15819931 35.067043304 -2.79182911 39.087852478 35.29537201 -1.86217856
		 -39.087852478 35.29537201 -1.86217856 36.69839478 35.54846191 -1.28123879 -36.69839478 35.54846191 -1.28123879
		 42.56097031 34.091617584 -3.27505302 -42.56097031 34.091617584 -3.27505302 42.58818436 32.47193909 -3.20427155
		 -42.58818436 32.47193909 -3.20427155 42.46981049 30.91530418 -3.85652208 -42.46981049 30.91530418 -3.85652208
		 42.41181564 30.07500267 -5.42977381 -42.41181564 30.07500267 -5.42977381 42.70219421 30.74627686 -7.25640392
		 -42.70219421 30.74627686 -7.25640392 42.57780457 32.34436035 -7.99932909 -42.57780457 32.34436035 -7.99932909
		 42.51325226 33.92261124 -8.16384029 -42.51325226 33.92261124 -8.16384029 42.51229477 35.53871918 -8.055926323
		 42.50421524 36.93835449 -7.90363455 -42.50421524 36.93835449 -7.90363455 -42.51229477 35.53871918 -8.055926323
		 40.80442047 39.70093918 -6.54462194 -40.80442047 39.70093918 -6.54462194 40.65943527 38.53578949 -7.82127857
		 -40.65943527 38.53578949 -7.82127857 38.91193008 40.07774353 -6.66470671 -38.91193008 40.07774353 -6.66470671
		 36.63218689 40.10301208 -5.72585487 -36.63218689 40.10301208 -5.72585487 40.66748428 36.70084763 -8.54326916
		 -40.66748428 36.70084763 -8.54326916 40.97040176 34.54194641 -8.6779604 -40.97040176 34.54194641 -8.6779604
		 38.51430893 38.64740372 -8.072126389 -38.51430893 38.64740372 -8.072126389 36.083015442 38.74792099 -7.94656181
		 -36.083015442 38.74792099 -7.94656181 38.38421631 36.60057068 -8.9335022 -38.38421631 36.60057068 -8.9335022
		 35.89090347 36.5477562 -9.040660858 -35.89090347 36.5477562 -9.040660858 38.38155746 34.32714462 -9.15638256
		 -38.38155746 34.32714462 -9.15638256 40.53025436 32.26575851 -8.53120041 -40.53025436 32.26575851 -8.53120041
		 38.24537277 31.97995758 -8.72205067 -38.24537277 31.97995758 -8.72205067 40.40443802 30.32491684 -7.46968842
		 -40.40443802 30.32491684 -7.46968842 38.2299118 30.014350891 -7.24933577 -38.2299118 30.014350891 -7.24933577
		 40.45064545 29.64466476 -5.27455235 -40.45064545 29.64466476 -5.27455235 38.33037949 29.37567902 -4.98142767
		 -38.33037949 29.37567902 -4.98142767 40.63763428 30.54530334 -3.30047727 -40.63763428 30.54530334 -3.30047727
		 38.61444473 30.40686989 -2.61398482 -38.61444473 30.40686989 -2.61398482 40.9242363 32.62384033 -2.4013288
		 -40.9242363 32.62384033 -2.4013288 38.88085938 32.81348801 -1.62998271 -38.88085938 32.81348801 -1.62998271
		 36.40649796 32.94062042 -1.1638608 -36.40649796 32.94062042 -1.1638608 36.15159225 30.40960693 -2.10310531
		 -36.15159225 30.40960693 -2.10310531 35.78903961 34.16339874 -9.24367714 -35.78903961 34.16339874 -9.24367714
		 35.73908615 31.77315521 -8.63021564 -35.73908615 31.77315521 -8.63021564 36.47460175 40.055721283 -3.47239733
		 -36.47460175 40.055721283 -3.47239733 33.7217865 39.68647003 -5.60322428 34.0022354126 39.55708313 -3.37674403
		 -34.0022354126 39.55708313 -3.37674403 -33.7217865 39.68647003 -5.60322428 34.2464447 37.97317123 -1.58483219
		 -34.2464447 37.97317123 -1.58483219 35.78272247 29.90096474 -6.97663212 -35.78272247 29.90096474 -6.97663212
		 35.87460709 29.33201599 -4.61386108 -35.87460709 29.33201599 -4.61386108 33.41390228 38.58649063 -7.80422115
		 33.18656158 36.48772049 -9.041885376 33.080440521 33.99385071 -9.28658867 33.04744339 31.58750153 -8.57636738
		 33.09254837 29.78296471 -6.70383835 33.23804474 29.36775398 -4.26757669 33.45370483 30.51899529 -1.7848376
		 33.71557617 32.99722672 -0.75533593 33.95967865 35.56581497 -0.82391316 -33.41390228 38.58649063 -7.80422115
		 -33.18656158 36.48772049 -9.041885376 -33.080440521 33.99385071 -9.28658867 -33.04744339 31.58750153 -8.57636738
		 -33.09254837 29.78296471 -6.70383835 -33.23804474 29.36775398 -4.26757669 -33.45370483 30.51899529 -1.7848376
		 -33.71557617 32.99722672 -0.75533593 -33.95967865 35.56581497 -0.82391316 17.60453224 26.69187164 -5.80805016
		 18.54928589 27.5219841 -1.59444129 -18.54928589 27.5219841 -1.59444129 -17.60453224 26.69187164 -5.80805016
		 17.29050636 30.030256271 -8.37278652 -17.29050636 30.030256271 -8.37278652 18.65417099 40.7739563 -0.18032014
		 -18.65417099 40.7739563 -0.18032014 18.48158073 40.94297409 -3.28659868;
	setAttr ".vt[1162:1327]" -18.48158073 40.94297409 -3.28659868 18.31518936 39.70187378 -6.37326431
		 -18.31518936 39.70187378 -6.37326431 17.46455956 36.88606644 -9.73660469 -17.46455956 36.88606644 -9.73660469
		 17.25981903 33.2515564 -9.72099018 -17.25981903 33.2515564 -9.72099018 16.3471489 23.034227371 -6.71016264
		 -16.3471489 23.034227371 -6.71016264 15.86162663 20.23166275 -6.58560991 -15.86162663 20.23166275 -6.58560991
		 15.67494297 17.62351418 -5.63920689 -15.67494297 17.62351418 -5.63920689 15.69732475 15.20752239 -3.43025446
		 -15.69732475 15.20752239 -3.43025446 16.069358826 14.91904926 0.37140885 -16.069358826 14.91904926 0.37140885
		 16.39888191 16.025625229 3.23365808 -16.39888191 16.025625229 3.23365808 17.8756218 43.27145386 0.57862049
		 -17.8756218 43.27145386 0.57862049 16.56375504 46.053565979 1.3180443 -16.56375504 46.053565979 1.3180443
		 15.47248173 36.5766449 -13.80676556 -15.47248173 36.5766449 -13.80676556 12.77470684 36.33982849 -17.15019417
		 -12.77470684 36.33982849 -17.15019417 15.42592907 32.79182434 -13.17033386 -15.42592907 32.79182434 -13.17033386
		 15.17007637 29.26691437 -11.97085285 -15.17007637 29.26691437 -11.97085285 14.91532326 25.87320137 -10.60567379
		 -14.91532326 25.87320137 -10.60567379 14.42212009 22.50872803 -10.11708736 -14.42212009 22.50872803 -10.11708736
		 14.16669369 19.43177223 -9.52392101 -14.16669369 19.43177223 -9.52392101 14.11923695 16.61040497 -8.57061195
		 -14.11923695 16.61040497 -8.57061195 12.66894341 32.34976959 -16.49874878 -12.66894341 32.34976959 -16.49874878
		 12.26947308 28.64688683 -15.35204887 -12.26947308 28.64688683 -15.35204887 11.84126282 25.23609924 -14.065242767
		 -11.84126282 25.23609924 -14.065242767 11.55708313 21.84631348 -13.16287994 -11.55708313 21.84631348 -13.16287994
		 11.53500843 18.52397919 -12.33918285 -11.53500843 18.52397919 -12.33918285 11.89805222 15.37560463 -11.30714989
		 -11.89805222 15.37560463 -11.30714989 9.13865376 36.2558136 -19.82324409 -9.13865376 36.2558136 -19.82324409
		 8.99446487 32.082893372 -19.17403412 -8.99446487 32.082893372 -19.17403412 8.60202217 28.26880646 -17.93564224
		 -8.60202217 28.26880646 -17.93564224 8.20532322 24.79018974 -16.57262802 -8.20532322 24.79018974 -16.57262802
		 8.072134018 21.25857162 -15.58985996 -8.072134018 21.25857162 -15.58985996 8.18240356 17.50286484 -14.73793221
		 -8.18240356 17.50286484 -14.73793221 8.54408932 13.16816711 -13.34919357 -8.54408932 13.16816711 -13.34919357
		 4.82996893 36.24727249 -21.59591866 -4.82996893 36.24727249 -21.59591866 4.71086025 31.9371376 -20.91554451
		 -4.71086025 31.9371376 -20.91554451 4.45425129 28.069929123 -19.5715065 -4.45425129 28.069929123 -19.5715065
		 4.19697189 24.55903053 -18.10631561 -4.19697189 24.55903053 -18.10631561 4.086476803 20.99705124 -17.10169792
		 -4.086476803 20.99705124 -17.10169792 4.10642719 17.20715714 -16.31822205 -4.10642719 17.20715714 -16.31822205
		 3.95191503 13.24400616 -15.0033254623 -3.95191503 13.24400616 -15.0033254623 0 36.24459839 -22.26802826
		 0 31.86839867 -21.59439087 0 27.97527313 -20.18515205 0 24.4982338 -18.65285683 0 20.98761559 -17.59996223
		 0 17.28781128 -16.63008118 0 13.77326202 -14.5989151 -15.095705986 16.26832199 9.97541428
		 -14.49285126 17.6580162 11.79296494 14.49285126 17.6580162 11.79296494 15.095705986 16.26832199 9.97541428
		 -13.19512844 17.037099838 13.41669655 13.19512844 17.037099838 13.41669655 -10.94528008 16.32473183 15.42694759
		 10.94528008 16.32473183 15.42694759 -7.89655018 15.63563824 17.35614014 7.89655018 15.63563824 17.35614014
		 -4.18978977 14.96786499 18.82278633 4.18978977 14.96786499 18.82278633 0 14.67001152 19.46473122
		 -13.47524166 14.56825924 11.64754581 13.47524166 14.56825924 11.64754581 -11.10664558 13.26635075 13.54378319
		 11.10664558 13.26635075 13.54378319 -8.017177582 12.17993546 15.29108524 8.017177582 12.17993546 15.29108524
		 -4.23294497 11.40296745 16.53318977 4.23294497 11.40296745 16.53318977 0 11.079679489 17.030618668
		 -15.51714039 14.98016453 7.31093931 15.51714039 14.98016453 7.31093931 -13.56640816 12.6052599 9.20388317
		 13.56640816 12.6052599 9.20388317 -11.044260025 10.82712269 11.10699558 11.044260025 10.82712269 11.10699558
		 -7.85818672 9.46914959 12.70392418 7.85818672 9.46914959 12.70392418 -4.086832523 8.61227989 13.70366669
		 4.086832523 8.61227989 13.70366669 0 8.32829666 14.046308517 15.45970535 13.52441502 4.57753134
		 -15.45970535 13.52441502 4.57753134 -13.26763248 10.92760944 6.44167042 13.26763248 10.92760944 6.44167042
		 -10.5500536 8.89667988 8.25928974 10.5500536 8.89667988 8.25928974 -7.40836906 7.46879244 9.53365326
		 7.40836906 7.46879244 9.53365326 -3.81960845 6.6567359 10.33530998 3.81960845 6.6567359 10.33530998
		 0 6.44478703 10.60208321 14.79677582 11.63239098 1.64897013 -14.79677582 11.63239098 1.64897013
		 -12.50343704 9.55920696 4.28020763 12.50343704 9.55920696 4.28020763 -9.72744846 7.93607044 5.65517282
		 9.72744846 7.93607044 5.65517282 -6.62901783 6.73735142 6.3566041 6.62901783 6.73735142 6.3566041
		 -3.85319042 6.20373249 4.028474331 3.85319042 6.20373249 4.028474331 0 6.027348518 3.86278796
		 -2.077862501 6.41342402 -0.15458618 0 6.49412537 -0.16370021 2.077862501 6.41342402 -0.15458618
		 0 7.83133888 -4.13887835 2.67010784 7.16817856 -4.85926151 -2.67010784 7.16817856 -4.85926151
		 0 9.47411633 -9.8643713 2.60635996 8.6705904 -10.11914539 -2.60635996 8.6705904 -10.11914539
		 0 11.0809021 -12.34440613 3.21748519 10.13584137 -12.75195503 -3.21748519 10.13584137 -12.75195503
		 -6.55848885 9.75590992 -11.72603989 6.55848885 9.75590992 -11.72603989 -4.82207537 8.50115681 -9.96564674
		 4.82207537 8.50115681 -9.96564674 11.34696293 10.97748089 -9.33830547 8.080047607 9.35831833 -9.18774223
		 -8.080047607 9.35831833 -9.18774223 -11.34696293 10.97748089 -9.33830547 -4.90396214 8.0079479218 -7.81437969
		 4.90396214 8.0079479218 -7.81437969 -7.87893391 6.61752653 -8.24450207 -4.70921516 5.93685913 -6.91240788
		 4.70921516 5.93685913 -6.91240788;
	setAttr ".vt[1328:1493]" 7.87893391 6.61752653 -8.24450207 -11.15689564 7.34375143 -8.080265045
		 11.15689564 7.34375143 -8.080265045 -13.77308083 7.6631279 -5.8757205 -14.11885262 14.08704567 -7.52396727
		 14.11885262 14.08704567 -7.52396727 13.77308083 7.6631279 -5.8757205 -14.74344826 7.74014664 -2.56318641
		 14.74344826 7.74014664 -2.56318641 -14.15902615 7.66319799 0.76260865 14.15902615 7.66319799 0.76260865
		 -12.32844067 7.061911583 3.30045915 12.32844067 7.061911583 3.30045915 -9.58146667 6.35809612 4.59511137
		 9.58146667 6.35809612 4.59511137 -2.70771337 5.40979052 -3.99375653 2.70771337 5.40979052 -3.99375653
		 -2.51989079 4.99862051 -0.28268334 2.51989079 4.99862051 -0.28268334 -4.40665483 5.12263203 3.042308569
		 4.40665483 5.12263203 3.042308569 -6.89433765 5.65608311 4.69361734 6.89433765 5.65608311 4.69361734
		 13.9039917 11.19886684 -6.6244297 15.21155548 11.50697517 -2.77915525 -13.9039917 11.19886684 -6.6244297
		 -15.21155548 11.50697517 -2.77915525 12.59033203 13.55911827 -9.86613846 -12.59033203 13.55911827 -9.86613846
		 -4.6123004 3.9908812 -6.66885281 -7.64957857 4.19239616 -7.98206091 7.64957857 4.19239616 -7.98206091
		 4.6123004 3.9908812 -6.66885281 -10.92061043 4.51267242 -7.50167084 10.92061043 4.51267242 -7.50167084
		 -13.40926743 4.72786522 -5.31429243 -14.3597784 4.84104681 -2.2982235 14.3597784 4.84104681 -2.2982235
		 13.40926743 4.72786522 -5.31429243 -13.78764343 4.88664961 -0.016166031 -12.11852169 4.75856733 2.59070158
		 12.11852169 4.75856733 2.59070158 13.78764343 4.88664961 -0.016166031 -9.71874428 4.4842267 3.88447285
		 9.71874428 4.4842267 3.88447285 -2.85287738 3.76205444 -3.65682721 2.85287738 3.76205444 -3.65682721
		 -2.89889479 3.79352832 0.12655935 2.89889479 3.79352832 0.12655935 -4.88033104 3.91983962 3.12638688
		 4.88033104 3.91983962 3.12638688 -7.17177916 4.20425987 4.14841938 7.17177916 4.20425987 4.14841938
		 -3.39324379 5.99453878 6.89531803 0 5.74707699 7.063380241 3.39324379 5.99453878 6.89531803
		 0 8.72270679 -7.14695835 2.64207172 7.90153885 -7.5510335 -2.64207172 7.90153885 -7.5510335
		 11.52517509 1.1920929e-06 2.91051936 11.62339878 1.42288351 3.14514589 7.63585949 1.42287993 4.40061235
		 7.71119499 -1.4156103e-06 3.87212706 -7.71119499 -1.4156103e-06 3.87212706 -7.63585949 1.42287993 4.40061235
		 -11.62339878 1.42288351 3.14514589 -11.52517509 1.1920929e-06 2.91051936 9.94519615 -1.3262033e-06 4.83684397
		 9.96681213 1.42288387 5.10142231 -9.94519615 -1.3262033e-06 4.83684397 -9.96681213 1.42288387 5.10142231
		 7.5369606 2.3698945 4.55741358 9.77451229 2.36989474 4.21480513 -9.77451229 2.36989474 4.21480513
		 -7.5369606 2.3698945 4.55741358 11.75798225 2.36989474 2.77348828 -11.75798225 2.36989474 2.77348828
		 6.099428177 1.42287803 4.99546862 6.27102518 -1.2665987e-06 4.49037361 -6.27102518 -1.2665987e-06 4.49037361
		 -6.099428177 1.42287803 4.99546862 4.082174778 1.42287743 3.44155502 4.1350112 -5.364418e-07 2.78746247
		 -4.1350112 -5.364418e-07 2.78746247 -4.082174778 1.42287743 3.44155502 5.48592949 2.36989474 3.74720144
		 -5.48592949 2.36989474 3.74720144 13.34165478 2.682209e-06 2.43752384 13.39530563 1.42288554 2.71672988
		 -13.39530563 1.42288554 2.71672988 -13.34165478 2.682209e-06 2.43752384 13.62431717 2.36989474 1.18163896
		 -13.62431717 2.36989474 1.18163896 14.47863579 1.42288303 0.16570097 14.15522957 2.36989474 -1.6437366
		 -14.15522957 2.36989474 -1.6437366 -14.47863579 1.42288303 0.16570097 3.22082734 2.36989474 0.30073142
		 -3.22082734 2.36989474 0.30073142 14.034215927 9.8347664e-07 0.2227561 -14.034215927 9.8347664e-07 0.2227561
		 3.66818166 2.0563602e-06 0.36053541 -3.66818166 2.0563602e-06 0.36053541 13.40858364 1.9669533e-06 -1.8150394
		 -13.40858364 1.9669533e-06 -1.8150394 12.44923878 2.4139881e-06 -4.10159779 13.052675247 2.36989021 -4.77796412
		 -13.052675247 2.36989021 -4.77796412 -12.44923878 2.4139881e-06 -4.10159779 10.32809258 3.2186508e-06 -6.26692629
		 10.68437767 2.36988854 -7.027781963 -10.68437767 2.36988854 -7.027781963 -10.32809258 3.2186508e-06 -6.26692629
		 7.56860971 2.36988759 -7.69386005 -7.56860971 2.36988759 -7.69386005 7.56071091 2.8610229e-06 -6.59285784
		 -7.56071091 2.8610229e-06 -6.59285784 4.68088675 2.36988878 -6.36438513 -4.68088675 2.36988878 -6.36438513
		 5.03326273 4.2915344e-06 -5.37886572 -5.03326273 4.2915344e-06 -5.37886572 2.93643808 2.36988807 -3.146106
		 -2.93643808 2.36988807 -3.146106 3.58269548 3.4570694e-06 -2.52184677 -3.58269548 3.4570694e-06 -2.52184677
		 9.86610031 -1.7136335e-06 -4.48211241 8.18773556 -7.1525574e-07 -4.35529137 -8.18773556 -7.1525574e-07 -4.35529137
		 -9.86610031 -1.7136335e-06 -4.48211241 6.56299686 2.0861626e-07 -3.96048665 -6.56299686 2.0861626e-07 -3.96048665
		 9.9271965 -4.1127205e-06 2.081745386 12.055706978 -3.7401915e-06 1.11450076 -12.055706978 -3.7401915e-06 1.11450076
		 -9.9271965 -4.1127205e-06 2.081745386 7.81165838 -3.9935112e-06 2.48760176 -7.81165838 -3.9935112e-06 2.48760176
		 7.041629314 -2.2612512e-06 0.077123731 9.16284943 -2.9914081e-06 -0.32719624 -9.16284943 -2.9914081e-06 -0.32719624
		 -7.041629314 -2.2612512e-06 0.077123731 11.31507969 -3.3378601e-06 -1.025290251 -11.31507969 -3.3378601e-06 -1.025290251
		 10.56600475 -2.6114285e-06 -3.033419371 8.58585548 -1.6465783e-06 -2.47093105 6.42684078 -7.8976154e-07 -2.1698246
		 -10.56600475 -2.6114285e-06 -3.033419371 -8.58585548 -1.6465783e-06 -2.47093105 -6.42684078 -7.8976154e-07 -2.1698246
		 0 33.29027939 17.72689056 -1.7847904 33.40523911 17.67931747 1.7847904 33.40523911 17.67931747
		 -3.36063981 33.33729935 17.45858192 3.36063981 33.33729935 17.45858192 -4.78914022 32.6227684 16.41991615
		 4.78914022 32.6227684 16.41991615 -5.65767479 31.12560844 15.56357098 5.65767479 31.12560844 15.56357098
		 -5.19929981 28.9008255 15.9037714 5.19929981 28.9008255 15.9037714 -4.0023908615 27.5372448 16.35392761
		 4.0023908615 27.5372448 16.35392761 -2.17255712 26.75273132 16.5363903 2.17255712 26.75273132 16.5363903
		 0 26.45014954 16.55012894 -2.25212336 28.74655151 13.32079124;
	setAttr ".vt[1494:1659]" 0 28.66786957 13.015325546 2.25212336 28.74655151 13.32079124
		 -2.15634251 31.043073654 13.4774847 0 31.017786026 13.23461437 2.15634251 31.043073654 13.4774847
		 -1.82972419 32.51207352 14.63401699 0 32.52415085 14.43910027 1.82972419 32.51207352 14.63401699
		 -1.6371479 33.3793602 16.028316498 0 33.40299606 15.93780899 1.6371479 33.3793602 16.028316498
		 -2.84822774 33.23500824 16.14858818 2.84822774 33.23500824 16.14858818 -3.4307797 32.55178452 15.21817684
		 3.4307797 32.55178452 15.21817684 -4.07127285 31.17464447 14.20199394 4.07127285 31.17464447 14.20199394
		 -4.05936718 28.88628197 14.2462635 4.05936718 28.88628197 14.2462635 -3.45054841 27.75773239 14.90992546
		 3.45054841 27.75773239 14.90992546 -2.0014300346 27.48513794 14.68072605 0 27.35157585 14.62175083
		 2.0014300346 27.48513794 14.68072605 -2.39152408 33.021385193 20.98856163 0 32.91862488 21.2168808
		 2.39152408 33.021385193 20.98856163 4.57595158 33.25162506 20.21404076 6.34092522 32.59901428 19.27066422
		 6.91173315 30.84431458 19.0078201294 6.20558119 29.38794518 19.55072594 4.62904024 28.31891632 20.40820503
		 2.54210711 27.57420158 21.18330383 0 27.28927422 21.49983978 -2.54210711 27.57420158 21.18330383
		 -4.62904024 28.31891632 20.40820503 -6.20558119 29.38794518 19.55072594 -6.91173315 30.84431458 19.0078201294
		 -6.34092522 32.59901428 19.27066422 -4.57595158 33.25162506 20.21404076 6.090939522 32.16359711 15.85580349
		 3.99715304 34.034088135 16.52197075 8.44701195 31.20914841 14.97524452 10.32418442 30.9474678 13.90528202
		 12.58623409 31.27786827 12.11238766 14.11973286 32.23804474 10.67191124 15.64935684 34.43283081 9.10165977
		 16.44399834 38.4778862 8.16589737 15.9268961 41.20464325 8.56028461 14.25498676 43.12551117 10.10079956
		 10.3917532 46.14363098 11.92874527 8.098761559 46.01884079 13.1649313 5.89093018 45.10857391 14.13945103
		 3.78015423 43.70402908 15.17650509 2.88893843 41.94964218 16.0018520355 2.4864316 39.64992905 16.60903358
		 2.66476727 36.77910233 16.74342728 -3.99714446 34.03408432 16.52196884 -6.090935707 32.16358948 15.85579586
		 -2.66476035 36.77910614 16.74343109 -2.48643732 39.64992905 16.60905075 -2.88893414 41.94963455 16.0018634796
		 -3.78014779 43.7040329 15.17650986 -5.89093018 45.10857391 14.13945103 -8.098761559 46.01884079 13.1649313
		 -10.3917532 46.14363098 11.92874527 -14.25498676 43.12551117 10.10079956 -15.9268961 41.20464325 8.56028461
		 -16.44398689 38.47787094 8.16589355 -15.6493454 34.43283081 9.10164738 -14.1197176 32.23803711 10.67190361
		 -12.58622646 31.27787018 12.11237812 -10.3241787 30.9474678 13.90527821 -8.44700718 31.2091465 14.97524261
		 6.8411293 33.58872604 15.099640846 5.2128458 35.043357849 15.61770153 8.67338467 32.84647751 14.4148531
		 10.13321304 32.64297485 13.5827713 11.8923502 32.8999176 12.18848419 13.0849123 33.6466217 11.068263054
		 14.27446079 35.35345078 9.84712029 14.89243221 38.49918365 9.11940193 14.47135448 40.62998581 9.42343712
		 13.32666874 42.609478 10.3359251 10.25930977 44.1293869 12.12721062 8.40925694 44.17602539 13.20442104
		 6.5137248 43.68491364 13.94010162 4.99762821 42.62836456 14.55844688 4.33607578 41.21995926 15.20906448
		 4.037997246 39.41065216 15.68540859 4.17668438 37.17808533 15.78992271 -5.21283913 35.043354034 15.61770058
		 -6.84112597 33.58872223 15.099635124 -4.17667913 37.17808914 15.78992558 -4.038002014 39.41065216 15.68542194
		 -4.33607292 41.21995544 15.20907307 -4.99762344 42.62836456 14.5584507 -6.5137248 43.68491364 13.94010162
		 -8.40925694 44.17602539 13.20442104 -10.25930977 44.1293869 12.12721062 -13.32666874 42.609478 10.3359251
		 -14.47135448 40.62998581 9.42343712 -14.89242363 38.49917221 9.11939907 -14.27445126 35.35345078 9.84710979
		 -13.084899902 33.64661789 11.068257332 -11.89234447 32.89992142 12.18847656 -10.13320827 32.64297485 13.58276844
		 -8.67338181 32.84647369 14.41485214 7.17963362 35.2714119 14.4960413 6.12472773 35.85682678 14.92787361
		 8.6152401 34.83938599 13.86632729 10.023141861 34.64188766 13.063344002 11.35578251 34.69026566 12.082740784
		 12.20046043 34.85052109 11.40702152 12.62970161 36.25383377 10.76619244 12.86068726 38.39510727 10.38298035
		 12.69634724 40.41465759 10.49189281 12.38204098 41.75921631 10.79524517 10.030806541 42.16637421 12.16615582
		 8.49222088 42.22103119 13.003865242 7.0090556145 42.078914642 13.67921448 5.89286757 41.77147293 14.1444273
		 5.71340132 40.66756058 14.4486227 5.65041447 39.049755096 14.73495674 5.75735188 37.25178528 14.85309124
		 -6.12472773 35.85682678 14.92787361 -7.17963362 35.2714119 14.4960413 -5.75735188 37.25178528 14.85309124
		 -5.65041447 39.049755096 14.73495674 -5.71340132 40.66756058 14.4486227 -5.89286757 41.77147293 14.1444273
		 -7.0090556145 42.078914642 13.67921448 -8.49222088 42.22103119 13.003865242 -10.030806541 42.16637421 12.16615582
		 -12.38204098 41.75921631 10.79524517 -12.69634724 40.41465759 10.49189281 -12.86068726 38.39510727 10.38298035
		 -12.62970161 36.25383377 10.76619244 -12.2004509 34.85051727 11.40701771 -11.35578251 34.69026566 12.082740784
		 -10.023141861 34.64188766 13.063344002 -8.6152401 34.83938599 13.86632729 7.09836483 36.97359467 14.083831787
		 7.066647053 38.77012634 13.89372158 7.056665421 40.48482513 13.71073341 -7.09836483 36.97359467 14.083831787
		 -7.066647053 38.77012634 13.89372158 -7.056665421 40.48482513 13.71073341 8.46231842 40.40916824 12.93625355
		 8.45802689 38.59078217 13.066576958 8.47819614 36.72138596 13.31664562 -8.46231842 40.40916824 12.93625355
		 -8.45802689 38.59078217 13.066576958 -8.47819614 36.72138596 13.31664562 9.91327763 40.34401321 12.10973167
		 9.87155628 38.47045517 12.21649361 9.87088776 36.55401993 12.48574543 -9.91327763 40.34401321 12.10973167
		 -9.87155628 38.47045517 12.21649361 -9.87088776 36.55401993 12.48574543 11.25498581 36.46134567 11.60657406
		 11.30725861 38.40161133 11.339324 11.31189728 40.28000259 11.29218292 11.40803719 41.93732452 11.37820244
		 11.84792042 43.50357056 11.1965065 12.58407402 44.83519745 11.13662052;
	setAttr ".vt[1660:1688]" 12.68201351 44.80162811 12.79462147 13.04494381 45.86050034 11.96320534
		 13.42699718 46.92872238 9.31202221 14.029497147 48.026679993 5.96525335 14.69914532 48.89966965 2.14389372
		 15.087634087 49.21073151 -2.037435532 14.99021721 48.71163177 -6.12357473 14.43981743 47.68266296 -9.63935852
		 13.8042717 46.65599823 -12.028191566 -11.25498581 36.46134567 11.60657406 -11.30725861 38.40161133 11.339324
		 -11.31189728 40.28000259 11.29218292 -11.40803719 41.93732452 11.37820244 -11.84792042 43.50357056 11.1965065
		 -12.58407402 44.83519745 11.13662052 -12.68201351 44.80162811 12.79462147 -13.04494381 45.86050034 11.96320534
		 -13.42699718 46.92872238 9.31202221 -14.029497147 48.026679993 5.96525335 -14.69914532 48.89966965 2.14389372
		 -15.087634087 49.21073151 -2.037435532 -14.99021721 48.71163177 -6.12357473 -14.43981743 47.68266296 -9.63935852
		 -13.8042717 46.65599823 -12.028191566 3.73656678 51.34309387 12.88933277 7.1484561 50.48713684 11.73733711
		 -7.1484561 50.48713684 11.73733711 -3.73656678 51.34309387 12.88933277 0 51.54742432 13.25376129;
	setAttr -s 3385 ".ed";
	setAttr ".ed[0:165]"  0 1 1 1 2 1 2 3 1 0 3 1 5 4 1 5 6 1 6 7 1 4 7 1 6 8 1
		 8 9 1 7 9 1 11 10 1 11 12 1 12 13 1 10 13 1 12 14 1 14 15 1 13 15 1 17 16 1 17 18 1
		 18 19 1 16 19 1 18 20 1 20 21 1 19 21 1 23 22 1 23 0 1 22 3 1 20 23 1 21 22 1 14 17 1
		 15 16 1 8 11 1 9 10 1 1 5 1 4 2 1 0 24 1 24 25 1 1 25 1 5 26 1 26 27 1 6 27 1 27 28 1
		 8 28 1 11 29 1 29 30 1 12 30 1 30 31 1 14 31 1 17 32 1 32 33 1 18 33 1 33 34 1 20 34 1
		 23 35 1 35 24 1 34 35 1 31 32 1 28 29 1 25 26 1 24 36 1 36 37 1 25 37 1 26 38 1 38 39 1
		 27 39 1 39 40 1 28 40 1 29 41 1 41 42 1 30 42 1 42 43 1 31 43 1 32 44 1 44 45 1 33 45 1
		 45 46 1 34 46 1 35 47 1 47 36 1 46 47 1 43 44 1 40 41 1 37 38 1 36 48 1 48 49 1 37 49 1
		 49 50 1 38 50 1 50 51 1 39 51 1 51 52 1 40 52 1 52 53 1 41 53 1 53 54 1 42 54 1 54 55 1
		 43 55 1 55 56 1 44 56 1 56 57 1 45 57 1 57 58 1 46 58 1 58 59 1 47 59 1 59 48 1 60 61 1
		 61 62 1 62 63 1 63 60 1 64 65 1 65 66 1 66 67 1 64 67 1 65 68 1 68 69 1 69 66 1 70 71 1
		 71 72 1 72 73 1 73 70 1 72 74 1 74 75 1 75 73 1 74 60 1 63 75 1 76 77 1 77 78 1 78 79 1
		 76 79 1 80 81 1 81 82 1 83 82 1 83 80 1 77 84 1 84 85 1 85 78 1 84 81 1 80 85 1 87 86 1
		 88 87 1 88 89 1 86 89 1 91 90 1 89 91 1 90 88 1 92 83 1 92 85 1 79 92 1 68 71 1 70 69 1
		 90 77 1 91 76 1 88 84 1 81 87 1 82 86 1 61 64 1 67 62 1 82 61 1 60 83 1 86 64 1 89 65 1
		 91 68 1 71 76 1;
	setAttr ".ed[166:331]" 79 72 1 92 74 1 93 94 1 94 95 1 95 96 1 96 93 1 97 94 1
		 93 98 1 98 97 1 99 100 1 100 97 1 98 99 1 101 102 1 102 100 1 99 101 1 103 104 1
		 104 105 1 106 105 1 106 103 1 108 107 1 108 104 1 103 107 1 96 109 1 109 110 1 110 93 1
		 111 101 1 99 112 1 112 111 1 109 113 1 113 114 1 114 110 1 113 111 1 112 114 1 114 98 1
		 107 115 1 103 116 1 115 116 1 117 106 1 116 117 1 95 108 1 107 96 1 115 109 1 116 113 1
		 111 117 1 101 106 1 105 102 1 94 118 1 118 119 1 119 95 1 119 120 1 120 108 1 120 121 1
		 104 121 1 121 122 1 105 122 1 122 123 1 102 123 1 123 124 1 100 124 1 124 125 1 97 125 1
		 125 118 1 126 127 1 127 128 1 128 129 1 129 126 1 130 131 1 131 132 1 132 133 1 130 133 1
		 131 134 1 134 135 1 135 132 1 136 137 1 137 138 1 138 139 1 139 136 1 137 140 1 140 141 1
		 141 138 1 140 127 1 126 141 1 142 143 1 144 143 1 144 145 1 145 142 1 146 147 1 147 148 1
		 148 149 1 149 146 1 151 150 1 151 147 1 146 150 1 148 142 1 145 149 1 152 144 1 152 149 1
		 150 152 1 154 153 1 155 154 1 155 156 1 153 156 1 157 155 1 158 157 1 156 158 1 134 136 1
		 139 135 1 158 151 1 147 157 1 155 148 1 142 154 1 143 153 1 128 130 1 133 129 1 127 144 1
		 143 128 1 153 130 1 156 131 1 158 134 1 136 151 1 150 137 1 152 140 1 121 132 1 135 122 1
		 120 133 1 159 118 1 125 160 1 160 159 1 124 161 1 161 160 1 75 159 1 160 73 1 67 162 1
		 162 163 1 62 163 1 159 164 1 164 119 1 165 120 1 164 165 1 166 164 1 166 167 1 167 165 1
		 163 166 1 163 168 1 168 167 1 162 169 1 169 168 1 170 133 1 165 170 1 171 129 1 170 171 1
		 172 126 1 171 172 1 173 141 1 172 173 1 174 138 1 173 174 1 175 139 1 174 175 1 176 135 1
		 175 176 1 122 177 1 177 178 1 123 178 1 178 161 1 176 177 1;
	setAttr ".ed[332:497]" 179 73 1 160 179 1 161 180 1 180 179 1 181 70 1 179 181 1
		 182 69 1 181 182 1 178 183 1 183 180 1 177 184 1 183 184 1 180 185 1 185 181 1 186 182 1
		 185 186 1 183 187 1 187 185 1 188 186 1 187 188 1 184 189 1 189 187 1 176 184 1 175 190 1
		 190 184 1 190 191 1 191 189 1 174 192 1 192 190 1 193 191 1 192 193 1 173 194 1 194 192 1
		 195 193 1 194 195 1 172 196 1 196 194 1 196 197 1 197 195 1 171 198 1 198 196 1 167 170 1
		 167 198 1 168 199 1 199 198 1 199 197 1 169 200 1 200 199 1 200 201 1 201 197 1 63 159 1
		 166 63 1 19 202 1 202 203 1 203 16 1 189 204 1 188 204 1 203 205 1 205 15 1 191 206 1
		 204 206 1 207 162 1 66 207 1 182 207 1 182 208 1 208 209 1 209 207 1 210 21 1 210 202 1
		 3 209 1 22 208 1 209 211 1 211 162 1 2 211 1 7 212 1 212 213 1 4 213 1 214 212 1
		 9 214 1 193 215 1 215 206 1 205 216 1 216 13 1 217 215 1 195 217 1 201 217 1 216 218 1
		 218 10 1 218 214 1 186 210 1 202 188 1 204 203 1 206 205 1 215 216 1 217 218 1 214 201 1
		 212 200 1 213 169 1 213 211 1 210 208 1 219 220 1 220 221 1 221 222 1 222 219 1 221 223 1
		 223 224 1 222 224 1 225 226 1 226 227 1 227 228 1 225 228 1 227 229 1 229 230 1 228 230 1
		 231 232 1 232 233 1 233 234 1 231 234 1 233 235 1 235 236 1 234 236 1 237 238 1 238 239 1
		 239 240 1 237 240 1 239 241 1 241 242 1 240 242 1 235 238 1 236 237 1 229 232 1 230 231 1
		 223 226 1 224 225 1 241 220 1 219 242 1 224 243 1 244 243 1 222 244 1 225 245 1 243 245 1
		 230 246 1 247 246 1 228 247 1 231 248 1 246 248 1 236 249 1 250 249 1 234 250 1 237 251 1
		 249 251 1 242 252 1 253 252 1 240 253 1 244 254 1 219 254 1 251 253 1 248 250 1 245 247 1
		 254 252 1 255 256 1 256 257 1 258 257 1 255 258 1 256 259 1 259 260 1;
	setAttr ".ed[498:663]" 257 260 1 261 262 1 262 263 1 264 263 1 261 264 1 262 265 1
		 265 266 1 263 266 1 267 268 1 268 269 1 270 269 1 267 270 1 268 271 1 271 272 1 269 272 1
		 273 274 1 274 275 1 276 275 1 273 276 1 277 255 1 258 278 1 277 278 1 271 273 1 272 276 1
		 265 267 1 266 270 1 259 261 1 260 264 1 274 277 1 278 275 1 254 277 1 252 274 1 244 255 1
		 243 256 1 245 259 1 247 261 1 246 262 1 248 265 1 250 267 1 249 268 1 251 271 1 253 273 1
		 279 280 1 280 281 1 281 282 1 282 279 1 282 283 1 283 284 1 284 279 1 283 285 1 285 286 1
		 286 284 1 285 287 1 287 288 1 288 286 1 289 290 1 290 291 1 291 292 1 289 292 1 290 293 1
		 294 293 1 294 291 1 288 295 1 295 296 1 296 286 1 297 298 1 298 280 1 279 297 1 299 300 1
		 300 298 1 297 299 1 295 300 1 299 296 1 284 299 1 301 294 1 302 301 1 302 291 1 292 303 1
		 303 302 1 292 288 1 287 289 1 303 295 1 300 302 1 298 301 1 280 294 1 293 281 1 281 304 1
		 293 305 1 305 304 1 304 306 1 306 282 1 307 283 1 306 307 1 308 285 1 307 308 1 309 287 1
		 308 309 1 309 310 1 310 289 1 311 290 1 310 311 1 311 305 1 312 313 1 313 314 1 314 315 1
		 315 312 1 316 317 1 318 317 1 318 319 1 319 316 1 320 321 1 321 316 1 319 320 1 322 323 1
		 323 324 1 324 325 1 325 322 1 326 327 1 327 323 1 322 326 1 314 327 1 326 315 1 328 329 1
		 329 330 1 330 331 1 328 331 1 332 333 1 334 333 1 334 335 1 335 332 1 336 337 1 337 332 1
		 335 336 1 329 337 1 336 330 1 339 338 1 340 339 1 341 340 1 341 338 1 343 342 1 338 343 1
		 342 341 1 336 344 1 331 344 1 344 334 1 317 313 1 312 318 1 339 329 1 340 328 1 337 338 1
		 332 343 1 333 342 1 324 321 1 320 325 1 317 340 1 313 328 1 331 314 1 327 344 1 323 334 1
		 333 324 1 321 342 1 316 341 1 345 346 1 346 347 1 347 348 1 348 345 1;
	setAttr ".ed[664:829]" 347 349 1 349 350 1 350 348 1 349 351 1 351 352 1 352 350 1
		 351 353 1 353 354 1 354 352 1 355 356 1 356 357 1 357 358 1 355 358 1 356 359 1 360 359 1
		 360 357 1 361 345 1 348 362 1 362 361 1 363 364 1 364 361 1 362 363 1 354 365 1 366 365 1
		 366 352 1 365 364 1 363 366 1 367 360 1 368 367 1 368 357 1 358 369 1 369 368 1 350 363 1
		 353 355 1 358 354 1 365 369 1 364 368 1 361 367 1 345 360 1 359 346 1 359 370 1 370 371 1
		 346 371 1 371 372 1 372 347 1 373 349 1 372 373 1 374 351 1 373 374 1 375 353 1 374 375 1
		 376 355 1 375 376 1 377 356 1 376 377 1 377 370 1 373 326 1 322 374 1 372 315 1 318 378 1
		 378 379 1 379 319 1 379 380 1 380 320 1 310 379 1 378 311 1 381 306 1 382 304 1 382 381 1
		 383 378 1 312 383 1 315 384 1 384 383 1 384 385 1 385 386 1 383 386 1 386 382 1 385 387 1
		 387 382 1 388 381 1 387 388 1 372 389 1 389 384 1 371 390 1 390 389 1 370 391 1 391 390 1
		 377 392 1 392 391 1 376 393 1 393 392 1 375 394 1 394 393 1 374 395 1 395 394 1 380 396 1
		 325 396 1 396 397 1 322 397 1 397 395 1 310 398 1 398 379 1 399 380 1 398 399 1 309 400 1
		 400 398 1 308 401 1 401 400 1 402 396 1 399 402 1 402 403 1 403 397 1 400 404 1 404 399 1
		 401 405 1 405 404 1 404 406 1 406 402 1 405 407 1 407 406 1 406 408 1 408 403 1 403 395 1
		 409 394 1 403 409 1 410 409 1 408 410 1 411 393 1 409 411 1 410 412 1 412 411 1 413 392 1
		 411 413 1 412 414 1 414 413 1 415 391 1 413 415 1 414 416 1 416 415 1 417 390 1 415 417 1
		 389 385 1 417 385 1 418 387 1 417 418 1 416 418 1 418 419 1 419 388 1 420 419 1 416 420 1
		 378 305 1 305 386 1 407 421 1 421 422 1 422 423 1 423 407 1 423 408 1 422 424 1 424 425 1
		 425 423 1 425 410 1 381 426 1 426 307 1 426 401 1 426 427 1 427 428 1;
	setAttr ".ed[830:995]" 428 401 1 429 405 1 429 421 1 428 239 1 427 241 1 430 427 1
		 381 430 1 430 220 1 419 431 1 431 432 1 388 432 1 433 431 1 420 433 1 434 412 1 425 434 1
		 424 435 1 435 434 1 434 436 1 436 414 1 436 420 1 435 437 1 437 436 1 437 433 1 238 429 1
		 421 235 1 233 422 1 232 424 1 229 435 1 227 437 1 433 226 1 431 223 1 432 221 1 430 432 1
		 428 429 1 438 1518 1 439 440 1 440 1519 1 441 438 1 443 441 1 440 442 1 442 1520 1
		 445 439 1 438 444 1 444 1533 1 442 446 1 446 1521 1 447 443 1 449 445 1 444 448 1
		 448 1532 1 446 450 1 450 1522 1 451 447 1 448 452 1 452 1531 1 453 449 1 455 451 1
		 450 454 1 454 1523 1 452 456 1 456 1530 1 457 453 1 459 455 1 454 458 1 458 1524 1
		 456 460 1 460 1529 1 461 457 1 463 459 1 458 462 1 462 1525 1 465 461 1 460 464 1
		 464 1528 1 462 466 1 466 1526 1 467 463 1 469 465 1 464 468 1 468 1527 1 466 469 1
		 468 467 1 471 470 1 470 447 1 451 471 1 473 472 1 472 448 1 444 473 1 475 471 1 451 474 1
		 474 475 1 472 476 1 476 477 1 477 448 1 455 478 1 478 474 1 479 452 1 477 479 1 459 480 1
		 480 478 1 479 481 1 481 456 1 483 482 1 482 467 1 468 483 1 484 483 1 464 484 1 482 485 1
		 485 463 1 486 484 1 460 486 1 485 480 1 481 486 1 489 487 1 487 488 1 488 485 1 485 489 1
		 492 490 1 490 486 1 486 491 1 491 492 1 494 493 1 493 482 1 483 494 1 495 494 1 484 495 1
		 493 489 1 490 495 1 479 496 1 497 478 1 498 496 1 477 498 1 497 499 1 499 474 1 500 480 1
		 488 500 1 481 501 1 501 491 1 500 497 1 496 501 1 502 475 1 499 502 1 476 503 1 503 498 1
		 470 504 1 504 505 1 505 447 1 507 473 1 444 506 1 506 507 1 505 508 1 508 443 1 438 509 1
		 509 506 1 510 441 1 508 510 1 510 509 1 510 511 1 511 512 1 512 509 1 508 513 1 513 511 1
		 514 506 1 512 514 1 505 515 1 515 513 1;
	setAttr ".ed[996:1161]" 514 516 1 516 507 1 517 515 1 504 517 1 511 518 1 512 519 1
		 518 519 1 513 520 1 520 518 1 514 521 1 519 521 1 515 522 1 522 520 1 516 523 1 521 523 1
		 517 524 1 524 522 1 518 525 1 519 526 1 525 526 1 520 527 1 527 525 1 521 528 1 526 528 1
		 522 529 1 529 527 1 523 530 1 528 530 1 524 531 1 531 529 1 533 530 1 528 532 1 532 533 1
		 531 534 1 534 535 1 535 529 1 537 533 1 532 536 1 536 537 1 534 538 1 538 539 1 539 535 1
		 536 540 1 540 541 1 541 537 1 543 539 1 538 542 1 542 543 1 545 1675 1 540 1676 1
		 544 545 1 542 1660 1 546 547 1 547 1661 1 544 548 1 548 549 1 549 545 1 551 547 1
		 546 550 1 550 551 1 553 549 1 548 552 1 552 553 1 550 554 1 554 555 1 555 551 1 557 553 1
		 552 556 1 556 557 1 554 558 1 558 559 1 559 555 1 561 557 1 556 560 1 560 561 1 558 562 1
		 562 563 1 563 559 1 565 561 1 560 564 1 564 565 1 562 566 1 566 567 1 567 563 1 564 498 1
		 503 565 1 499 567 1 566 502 1 525 568 1 568 569 1 569 526 1 527 570 1 570 568 1 571 572 1
		 573 571 1 571 574 1 572 575 1 574 575 1 573 576 1 576 574 1 574 577 1 575 578 1 577 578 1
		 576 579 1 579 577 1 577 580 1 578 581 1 580 581 1 579 582 1 582 580 1 580 583 1 581 584 1
		 583 584 1 582 585 1 585 583 1 583 586 1 584 587 1 586 587 1 585 588 1 588 586 1 586 589 1
		 587 590 1 589 590 1 588 591 1 591 589 1 535 570 1 569 532 1 569 592 1 592 536 1 593 570 1
		 539 593 1 592 594 1 594 540 1 595 593 1 543 595 1 572 596 1 597 573 1 596 598 1 598 594 1
		 595 599 1 599 597 1 594 1677 1 600 544 1 595 1662 1 547 601 1 598 1678 1 602 600 1
		 599 1663 1 601 603 1 600 604 1 604 548 1 601 605 1 551 605 1 606 604 1 605 607 1
		 604 608 1 608 552 1 605 609 1 555 609 1 606 610 1 610 608 1 607 611 1 609 611 1 608 612 1
		 612 556 1 609 613 1;
	setAttr ".ed[1162:1327]" 559 613 1 610 614 1 614 612 1 611 615 1 613 615 1 612 616 1
		 616 560 1 613 617 1 563 617 1 614 618 1 618 616 1 615 619 1 617 619 1 616 620 1 620 621 1
		 617 623 1 622 623 1 618 624 1 624 620 1 619 625 1 623 625 1 563 622 1 560 621 1 567 626 1
		 626 622 1 621 627 1 627 564 1 497 626 1 627 496 1 620 628 1 621 629 1 628 629 1 622 630 1
		 623 631 1 630 631 1 624 632 1 632 628 1 625 633 1 631 633 1 626 634 1 634 630 1 627 635 1
		 629 635 1 500 634 1 635 501 1 628 636 1 629 637 1 636 637 1 630 638 1 631 639 1 638 639 1
		 632 640 1 640 636 1 633 641 1 639 641 1 634 642 1 642 638 1 635 643 1 637 643 1 488 642 1
		 643 491 1 636 644 1 637 645 1 644 645 1 638 646 1 639 647 1 646 647 1 640 648 1 648 644 1
		 641 649 1 647 649 1 642 650 1 650 646 1 643 651 1 645 651 1 487 650 1 651 492 1 602 606 1
		 607 603 1 652 599 1 598 653 1 652 654 1 654 597 1 655 653 1 596 655 1 654 576 1 575 655 1
		 652 656 1 656 657 1 657 654 1 655 658 1 658 659 1 659 653 1 657 579 1 578 658 1 656 660 1
		 657 661 1 660 661 1 658 662 1 659 663 1 662 663 1 661 582 1 581 662 1 660 664 1 661 665 1
		 664 665 1 662 666 1 663 667 1 666 667 1 665 585 1 584 666 1 656 1665 1 659 1680 1
		 660 1666 1 668 670 1 663 1681 1 669 671 1 664 1667 1 670 672 1 667 1682 1 671 673 1
		 674 668 1 669 675 1 676 670 1 674 676 1 671 677 1 677 675 1 676 678 1 678 672 1 673 679 1
		 679 677 1 665 680 1 680 588 1 587 681 1 681 666 1 664 682 1 682 680 1 681 683 1 683 667 1
		 672 684 1 684 1668 1 685 673 1 683 1683 1 678 686 1 686 684 1 687 679 1 685 687 1
		 680 688 1 688 591 1 689 681 1 590 689 1 682 690 1 690 688 1 691 683 1 689 691 1 684 690 1
		 691 685 1 686 692 1 692 690 1 693 687 1 691 693 1 589 694 1 590 695 1 694 695 1 591 696 1
		 696 694 1;
	setAttr ".ed[1328:1493]" 688 697 1 697 696 1 689 698 1 695 698 1 692 697 1 698 693 1
		 700 699 1 727 743 1 702 701 1 729 746 1 700 703 1 699 704 1 703 704 1 702 705 1 701 706 1
		 705 706 1 703 739 1 704 740 1 707 708 1 705 742 1 706 741 1 709 710 1 707 711 1 708 712 1
		 711 712 1 709 713 1 710 714 1 713 714 1 711 735 1 712 736 1 715 716 1 713 738 1 714 737 1
		 717 718 1 715 719 1 716 720 1 719 720 1 717 721 1 718 722 1 721 722 1 719 731 1 720 732 1
		 723 724 1 721 734 1 722 733 1 725 726 1 723 727 1 724 728 1 727 728 1 725 729 1 726 730 1
		 729 730 1 699 744 1 701 745 1 731 723 1 732 724 1 731 732 1 733 726 1 734 725 1 733 734 1
		 735 715 1 736 716 1 735 736 1 737 718 1 738 717 1 737 738 1 739 707 1 740 708 1 739 740 1
		 741 710 1 742 709 1 741 742 1 743 700 1 744 728 1 743 744 1 745 730 1 746 702 1 745 746 1
		 727 747 1 743 748 1 747 748 1 700 749 1 703 750 1 749 750 1 701 751 1 706 752 1 751 752 1
		 739 753 1 750 753 1 741 754 1 752 754 1 707 755 1 711 756 1 755 756 1 710 757 1 714 758 1
		 757 758 1 735 759 1 756 759 1 737 760 1 758 760 1 715 761 1 719 762 1 761 762 1 718 763 1
		 722 764 1 763 764 1 731 765 1 762 765 1 733 766 1 764 766 1 723 767 1 767 747 1 726 768 1
		 730 769 1 768 769 1 745 770 1 751 770 1 765 767 1 766 768 1 759 761 1 760 763 1 753 755 1
		 754 757 1 748 749 1 770 769 1 747 795 1 748 796 1 771 772 1 749 797 1 750 798 1 773 774 1
		 751 809 1 752 810 1 775 776 1 753 799 1 774 777 1 754 811 1 776 778 1 755 800 1 756 801 1
		 779 780 1 757 812 1 758 813 1 781 782 1 759 802 1 780 783 1 760 814 1 782 784 1 761 803 1
		 762 804 1 785 786 1 763 815 1 764 816 1 787 788 1 765 805 1 786 789 1 766 817 1 788 790 1
		 767 806 1 791 771 1 768 818 1 769 807 1 792 793 1 770 808 1 775 794 1;
	setAttr ".ed[1494:1659]" 789 791 1 790 792 1 783 785 1 784 787 1 777 779 1 778 781 1
		 772 773 1 794 793 1 795 771 1 796 772 1 797 773 1 798 774 1 799 777 1 800 779 1 801 780 1
		 802 783 1 803 785 1 804 786 1 805 789 1 806 791 1 795 796 1 796 797 1 797 798 1 798 799 1
		 799 800 1 800 801 1 801 802 1 802 803 1 803 804 1 804 805 1 805 806 1 806 795 1 807 793 1
		 808 794 1 809 775 1 810 776 1 811 778 1 812 781 1 813 782 1 814 784 1 815 787 1 816 788 1
		 817 790 1 818 792 1 807 808 1 808 809 1 809 810 1 810 811 1 811 812 1 812 813 1 813 814 1
		 814 815 1 815 816 1 816 817 1 817 818 1 818 807 1 439 819 1 440 820 1 819 820 1 442 821 1
		 820 821 1 445 822 1 822 819 1 446 823 1 821 823 1 449 824 1 824 822 1 450 825 1 823 825 1
		 453 826 1 826 824 1 454 827 1 825 827 1 457 828 1 828 826 1 458 829 1 827 829 1 461 830 1
		 830 828 1 462 831 1 829 831 1 465 832 1 832 830 1 466 833 1 831 833 1 469 834 1 834 832 1
		 833 834 1 835 885 1 837 887 1 839 895 1 841 893 1 844 888 1 843 838 1 837 844 1 846 896 1
		 845 840 1 839 846 1 848 889 1 847 843 1 844 848 1 850 897 1 849 845 1 846 850 1 851 891 1
		 853 899 1 856 892 1 855 852 1 851 856 1 858 900 1 857 854 1 853 858 1 856 836 1 835 855 1
		 858 842 1 841 857 1 852 871 1 848 872 1 850 878 1 854 879 1 838 877 1 836 876 1 842 884 1
		 840 883 1 860 873 1 859 851 1 848 860 1 862 880 1 861 849 1 854 862 1 864 875 1 863 837 1
		 836 864 1 866 882 1 865 841 1 840 866 1 860 867 1 864 868 1 862 870 1 866 869 1 867 863 1
		 868 859 1 867 874 1 869 861 1 870 865 1 869 881 1 867 844 1 845 869 1 856 868 1 870 857 1
		 871 847 1 872 851 1 873 859 1 874 868 1 875 863 1 876 837 1 877 835 1 871 890 1 872 873 1
		 873 874 1 874 875 1 875 876 1 876 886 1 878 853 1 879 849 1 880 861 1;
	setAttr ".ed[1660:1825]" 881 870 1 882 865 1 883 841 1 884 839 1 878 898 1 879 880 1
		 880 881 1 881 882 1 882 883 1 883 894 1 885 836 1 886 877 1 887 838 1 888 843 1 889 847 1
		 890 872 1 891 852 1 892 855 1 885 886 1 886 887 1 887 888 1 888 889 1 889 890 1 890 891 1
		 891 892 1 892 885 1 893 842 1 894 884 1 895 840 1 896 845 1 897 849 1 898 879 1 899 854 1
		 900 857 1 893 894 1 894 895 1 895 896 1 896 897 1 897 898 1 898 899 1 899 900 1 900 893 1
		 904 951 1 901 937 1 902 953 1 903 938 1 908 959 1 905 945 1 906 961 1 907 944 1 904 909 1
		 909 958 1 910 901 1 908 911 1 911 966 1 912 905 1 914 957 1 913 910 1 909 914 1 916 965 1
		 915 912 1 911 916 1 918 955 1 917 943 1 914 942 1 920 963 1 919 949 1 916 950 1 922 954 1
		 921 917 1 918 922 1 924 962 1 923 919 1 920 924 1 902 921 1 922 903 1 906 923 1 924 907 1
		 926 939 1 925 904 1 903 926 1 928 946 1 927 906 1 905 928 1 914 929 1 929 941 1 930 918 1
		 919 931 1 931 948 1 932 915 1 926 933 1 929 934 1 928 936 1 931 935 1 933 930 1 934 925 1
		 933 940 1 935 927 1 936 932 1 935 947 1 934 909 1 912 936 1 922 933 1 935 923 1 937 902 1
		 938 904 1 939 925 1 940 934 1 941 930 1 942 918 1 943 913 1 937 952 1 938 939 1 939 940 1
		 940 941 1 941 942 1 942 956 1 944 908 1 945 906 1 946 927 1 947 936 1 948 932 1 949 915 1
		 950 920 1 944 960 1 945 946 1 946 947 1 947 948 1 948 949 1 949 964 1 951 901 1 952 938 1
		 953 903 1 954 921 1 955 917 1 956 943 1 957 913 1 958 910 1 951 952 1 952 953 1 953 954 1
		 954 955 1 955 956 1 956 957 1 957 958 1 958 951 1 959 905 1 960 945 1 961 907 1 962 923 1
		 963 919 1 964 950 1 965 915 1 966 912 1 959 960 1 960 961 1 961 962 1 962 963 1 963 964 1
		 964 965 1 965 966 1 966 959 1 970 1017 1 967 1008 1 968 1019 1 969 1009 1;
	setAttr ".ed[1826:1991]" 974 1025 1 971 1016 1 972 1027 1 973 1015 1 976 1020 1
		 975 969 1 968 976 1 978 1028 1 977 973 1 972 978 1 980 1021 1 979 975 1 976 980 1
		 982 1029 1 981 977 1 978 982 1 984 1023 1 983 1003 1 980 1004 1 986 1031 1 985 1011 1
		 982 1010 1 988 1024 1 987 983 1 984 988 1 990 1032 1 989 985 1 986 990 1 970 987 1
		 988 967 1 974 989 1 990 971 1 967 991 1 991 1007 1 992 968 1 973 993 1 993 1014 1
		 994 974 1 996 1005 1 995 999 1 991 1000 1 998 1012 1 997 1002 1 993 1001 1 980 995 1
		 996 984 1 985 997 1 998 981 1 999 992 1 1000 996 1 999 1006 1 1001 998 1 1002 994 1
		 1001 1013 1 988 1000 1 1002 989 1 999 976 1 977 1001 1 1003 979 1 1004 984 1 1005 995 1
		 1006 1000 1 1007 992 1 1008 968 1 1009 970 1 1003 1022 1 1004 1005 1 1005 1006 1
		 1006 1007 1 1007 1008 1 1008 1018 1 1010 986 1 1011 981 1 1012 997 1 1013 1002 1
		 1014 994 1 1015 974 1 1016 972 1 1010 1030 1 1011 1012 1 1012 1013 1 1013 1014 1
		 1014 1015 1 1015 1026 1 1017 967 1 1018 1009 1 1019 969 1 1020 975 1 1021 979 1 1022 1004 1
		 1023 983 1 1024 987 1 1017 1018 1 1018 1019 1 1019 1020 1 1020 1021 1 1021 1022 1
		 1022 1023 1 1023 1024 1 1024 1017 1 1025 971 1 1026 1016 1 1027 973 1 1028 977 1
		 1029 981 1 1030 1011 1 1031 985 1 1032 989 1 1025 1026 1 1026 1027 1 1027 1028 1
		 1028 1029 1 1029 1030 1 1030 1031 1 1031 1032 1 1032 1025 1 921 975 1 979 917 1 978 924 1
		 920 982 1 902 969 1 972 907 1 1034 901 1 910 1033 1 1033 1034 1 908 1035 1 1035 1036 1
		 1036 911 1 913 1037 1 1037 1033 1 1036 1038 1 1038 916 1 855 1034 1 1033 852 1 853 1036 1
		 1035 858 1 877 1039 1 1039 1040 1 1041 1042 1 1042 884 1 838 1043 1 1043 1039 1 1044 839 1
		 1042 1044 1 1034 1045 1 1045 937 1 1046 1035 1 944 1046 1 1047 902 1 1045 1047 1
		 907 1048 1 1048 1046 1 1040 1045 1 1046 1041 1 1040 1049 1 1049 1047 1 1048 1050 1
		 1050 1041 1 1039 1051 1 1051 1049 1 1050 1052 1 1052 1042 1 1043 1053 1 1053 1051 1
		 1054 1044 1 1052 1054 1;
	setAttr ".ed[1992:2157]" 1055 969 1 1047 1055 1 972 1056 1 1056 1048 1 1057 1009 1
		 1055 1057 1 1016 1058 1 1058 1056 1 1059 970 1 1057 1059 1 971 1060 1 1060 1058 1
		 1061 987 1 1059 1061 1 990 1062 1 1062 1060 1 1063 983 1 1061 1063 1 986 1064 1 1064 1062 1
		 1065 1003 1 1063 1065 1 1010 1066 1 1066 1064 1 1067 979 1 1065 1067 1 982 1068 1
		 1068 1066 1 917 1069 1 943 1070 1 1069 1070 1 950 1071 1 1038 1071 1 1070 1037 1
		 920 1072 1 1071 1072 1 1067 1069 1 1072 1068 1 1073 852 1 1033 1073 1 853 1074 1
		 1074 1036 1 1037 1075 1 1075 1073 1 1076 1038 1 1074 1076 1 1077 871 1 1073 1077 1
		 878 1078 1 1078 1074 1 1079 847 1 1077 1079 1 850 1080 1 1080 1078 1 1070 1081 1
		 1081 1075 1 1082 1071 1 1076 1082 1 1081 1083 1 1069 1083 1 1082 1084 1 1084 1072 1
		 1075 1085 1 1085 1077 1 1078 1086 1 1086 1076 1 1087 1079 1 1085 1087 1 1080 1088 1
		 1088 1086 1 1081 1089 1 1089 1085 1 1086 1090 1 1090 1082 1 1091 1087 1 1089 1091 1
		 1088 1092 1 1092 1090 1 1083 1093 1 1093 1089 1 1090 1094 1 1094 1084 1 1067 1083 1
		 1084 1068 1 1065 1095 1 1095 1083 1 1096 1066 1 1084 1096 1 1095 1097 1 1097 1093 1
		 1098 1096 1 1094 1098 1 1063 1099 1 1099 1095 1 1100 1064 1 1096 1100 1 1101 1097 1
		 1099 1101 1 1098 1102 1 1102 1100 1 1061 1103 1 1103 1099 1 1104 1062 1 1100 1104 1
		 1105 1101 1 1103 1105 1 1102 1106 1 1106 1104 1 1059 1107 1 1107 1103 1 1108 1060 1
		 1104 1108 1 1107 1109 1 1109 1105 1 1106 1110 1 1110 1108 1 1057 1111 1 1111 1107 1
		 1112 1058 1 1108 1112 1 1049 1055 1 1056 1050 1 1049 1111 1 1112 1050 1 1051 1113 1
		 1113 1111 1 1114 1052 1 1112 1114 1 1113 1109 1 1110 1114 1 1053 1115 1 1115 1113 1
		 1114 1116 1 1116 1054 1 1115 1117 1 1117 1109 1 1118 1116 1 1110 1118 1 835 1034 1
		 1035 842 1 1040 835 1 842 1041 1 720 1136 1 1091 1119 1 1119 1137 1 1092 1145 1 717 1146 1
		 1120 1092 1 1093 1119 1 1120 1094 1 1121 1138 1 1119 1121 1 738 1147 1 1122 1120 1
		 1097 1121 1 1122 1098 1 1123 1043 1 843 1123 1 1044 1124 1 1124 846 1 1079 1123 1
		 1124 1080 1 1079 1125 1 1125 1126 1 1126 1123 1 1124 1127 1;
	setAttr ".ed[2158:2323]" 1127 1128 1 1128 1080 1 724 1125 1 1087 1135 1 1128 725 1
		 734 1144 1 728 1126 1 1127 729 1 1126 1129 1 1129 1043 1 1130 1127 1 1044 1130 1
		 744 1129 1 1130 746 1 699 1143 1 1054 1152 1 704 1142 1 1116 1151 1 740 1141 1 1118 1150 1
		 1101 1131 1 1131 1121 1 1132 1102 1 1122 1132 1 1131 1139 1 713 1148 1 1133 1131 1
		 1105 1133 1 1132 1134 1 1134 1106 1 1117 1133 1 1134 1118 1 1133 1140 1 709 1149 1
		 1135 732 1 1136 1091 1 1137 716 1 1138 736 1 1139 712 1 1140 708 1 1141 1117 1 1142 1115 1
		 1143 1053 1 1135 1136 1 1136 1137 1 1137 1138 1 1138 1139 1 1139 1140 1 1140 1141 1
		 1141 1142 1 1142 1143 1 1144 1088 1 1145 721 1 1146 1120 1 1147 1122 1 1148 1132 1
		 1149 1134 1 1150 742 1 1151 705 1 1152 702 1 1144 1145 1 1145 1146 1 1146 1147 1
		 1147 1148 1 1148 1149 1 1149 1150 1 1150 1151 1 1151 1152 1 1143 1129 1 1130 1152 1
		 1135 1125 1 1128 1144 1 1154 59 1 58 1153 1 1153 1154 1 276 1155 1 1155 1156 1 1156 272 1
		 1154 625 1 624 1155 1 779 1154 1 1153 780 1 782 1156 1 1155 781 1 777 625 1 624 778 1
		 1153 1157 1 1157 783 1 1158 1156 1 784 1158 1 49 633 1 641 50 1 258 640 1 632 278 1
		 649 51 1 257 648 1 48 625 1 624 275 1 774 619 1 618 776 1 773 615 1 614 775 1 772 611 1
		 610 794 1 1159 611 1 771 1159 1 610 1160 1 1160 793 1 1161 1159 1 791 1161 1 1160 1162 1
		 1162 792 1 789 1163 1 1163 1161 1 1162 1164 1 1164 790 1 786 1165 1 1165 1163 1 1164 1166 1
		 1166 788 1 1167 1165 1 785 1167 1 1166 1168 1 1168 787 1 1157 1167 1 1168 1158 1
		 1169 1153 1 57 1169 1 1156 1170 1 1170 269 1 1171 1169 1 56 1171 1 1170 1172 1 1172 270 1
		 1173 1171 1 55 1173 1 1172 1174 1 1174 266 1 54 1175 1 1175 1173 1 1174 1176 1 1176 263 1
		 1177 1175 1 53 1177 1 1176 1178 1 1178 264 1 1179 1177 1 52 1179 1 1178 1180 1 1180 260 1
		 649 1179 1 1180 648 1 1181 607 1 1159 1181 1 606 1182 1 1182 1160 1 1181 1183 1 1183 603 1
		 1184 1182 1 602 1184 1 1183 1664 1 653 1679 1 1183 668 1 669 1184 1 1181 674 1 675 1182 1;
	setAttr ".ed[2324:2489]" 1161 674 1 675 1162 1 1163 676 1 677 1164 1 1165 678 1
		 679 1166 1 1185 686 1 1165 1185 1 687 1186 1 1186 1166 1 1187 692 1 1185 1187 1 693 1188 1
		 1188 1186 1 1167 1189 1 1189 1185 1 1186 1190 1 1190 1168 1 1191 1189 1 1157 1191 1
		 1190 1192 1 1192 1158 1 1153 1193 1 1193 1191 1 1192 1194 1 1194 1156 1 1169 1195 1
		 1195 1193 1 1196 1170 1 1194 1196 1 1171 1197 1 1197 1195 1 1198 1172 1 1196 1198 1
		 1199 1197 1 1173 1199 1 1198 1200 1 1200 1174 1 1189 1201 1 1201 1187 1 1190 1202 1
		 1188 1202 1 1191 1203 1 1203 1201 1 1192 1204 1 1202 1204 1 1193 1205 1 1205 1203 1
		 1194 1206 1 1204 1206 1 1195 1207 1 1207 1205 1 1196 1208 1 1206 1208 1 1197 1209 1
		 1209 1207 1 1198 1210 1 1208 1210 1 1199 1211 1 1211 1209 1 1200 1212 1 1210 1212 1
		 1187 1213 1 1213 697 1 1188 1214 1 698 1214 1 1201 1215 1 1215 1213 1 1202 1216 1
		 1214 1216 1 1203 1217 1 1217 1215 1 1204 1218 1 1216 1218 1 1205 1219 1 1219 1217 1
		 1206 1220 1 1218 1220 1 1207 1221 1 1221 1219 1 1208 1222 1 1220 1222 1 1209 1223 1
		 1223 1221 1 1210 1224 1 1222 1224 1 1211 1225 1 1225 1223 1 1212 1226 1 1224 1226 1
		 1213 1227 1 1227 696 1 1214 1228 1 695 1228 1 1215 1229 1 1229 1227 1 1216 1230 1
		 1228 1230 1 1217 1231 1 1231 1229 1 1218 1232 1 1230 1232 1 1219 1233 1 1233 1231 1
		 1220 1234 1 1232 1234 1 1221 1235 1 1235 1233 1 1222 1236 1 1234 1236 1 1223 1237 1
		 1237 1235 1 1224 1238 1 1236 1238 1 1225 1239 1 1239 1237 1 1226 1240 1 1238 1240 1
		 1227 1241 1 1241 694 1 1228 1241 1 1229 1242 1 1242 1241 1 1230 1242 1 1231 1243 1
		 1243 1242 1 1232 1243 1 1233 1244 1 1244 1243 1 1234 1244 1 1235 1245 1 1245 1244 1
		 1236 1245 1 1237 1246 1 1246 1245 1 1238 1246 1 1239 1247 1 1247 1246 1 1240 1247 1
		 644 1248 1 1248 1249 1 1249 645 1 1251 647 1 646 1250 1 1250 1251 1 1249 1252 1 1252 651 1
		 650 1253 1 1253 1250 1 1254 492 1 1252 1254 1 487 1255 1 1255 1253 1 1256 490 1 1254 1256 1
		 489 1257 1 1257 1255 1 1258 495 1 1256 1258 1 493 1259 1 1259 1257 1 1258 1260 1
		 1260 494 1 1260 1259 1 1252 1261 1 1248 1261 1;
	setAttr ".ed[2490:2655]" 1253 1262 1 1262 1251 1 1254 1263 1 1261 1263 1 1255 1264 1
		 1264 1262 1 1256 1265 1 1263 1265 1 1257 1266 1 1266 1264 1 1258 1267 1 1265 1267 1
		 1259 1268 1 1268 1266 1 1260 1269 1 1267 1269 1 1269 1268 1 648 1270 1 1270 1248 1
		 1251 1271 1 1271 649 1 1270 1272 1 1272 1261 1 1273 1271 1 1262 1273 1 1274 1263 1
		 1272 1274 1 1264 1275 1 1275 1273 1 1276 1265 1 1274 1276 1 1266 1277 1 1277 1275 1
		 1278 1267 1 1276 1278 1 1268 1279 1 1279 1277 1 1280 1269 1 1278 1280 1 1280 1279 1
		 1271 1281 1 1281 1179 1 1180 1282 1 1282 1270 1 1272 1283 1 1282 1283 1 1273 1284 1
		 1284 1281 1 1274 1285 1 1283 1285 1 1275 1286 1 1286 1284 1 1276 1287 1 1285 1287 1
		 1277 1288 1 1288 1286 1 1278 1289 1 1287 1289 1 1279 1290 1 1290 1288 1 1280 1291 1
		 1289 1291 1 1291 1290 1 1281 1292 1 1292 1177 1 1282 1293 1 1178 1293 1 1283 1294 1
		 1293 1294 1 1284 1295 1 1295 1292 1 1285 1296 1 1294 1296 1 1286 1297 1 1297 1295 1
		 1287 1298 1 1296 1298 1 1288 1299 1 1299 1297 1 1289 1381 1 1298 1300 1 1290 1383 1
		 1301 1299 1 1291 1382 1 1300 1302 1 1302 1301 1 1300 1303 1 1303 1304 1 1304 1302 1
		 1305 1301 1 1304 1305 1 1304 1306 1 1306 1307 1 1307 1305 1 1303 1308 1 1308 1306 1
		 1309 1310 1 1311 1309 1 1313 1310 1 1309 1312 1 1312 1313 1 1311 1314 1 1314 1312 1
		 1239 1313 1 1312 1247 1 1314 1240 1 1315 1226 1 1314 1315 1 1225 1316 1 1316 1313 1
		 1317 1315 1 1311 1317 1 1316 1318 1 1318 1310 1 1320 1316 1 1225 1319 1 1319 1320 1
		 1315 1321 1 1321 1322 1 1322 1226 1 1317 1323 1 1323 1321 1 1320 1324 1 1324 1318 1
		 1308 1323 1 1324 1307 1 1326 1325 1 1325 1321 1 1323 1326 1 1328 1327 1 1327 1324 1
		 1320 1328 1 1329 1322 1 1325 1329 1 1319 1330 1 1330 1328 1 1331 1353 1 1333 1351 1
		 1332 1200 1 1199 1333 1 1332 1176 1 1175 1333 1 1331 1335 1 1335 1354 1 1336 1334 1
		 1175 1352 1 1337 1293 1 1292 1338 1 1337 1339 1 1339 1294 1 1295 1340 1 1340 1338 1
		 1339 1341 1 1341 1296 1 1342 1340 1 1297 1342 1 1343 1326 1 1308 1343 1 1327 1344 1
		 1344 1307 1 1303 1345 1 1345 1343 1 1344 1346 1 1346 1305 1 1347 1345 1 1300 1347 1;
	setAttr ".ed[2656:2821]" 1346 1348 1 1348 1301 1 1298 1349 1 1349 1347 1 1348 1350 1
		 1350 1299 1 1341 1349 1 1350 1342 1 1351 1334 1 1352 1336 1 1351 1352 1 1353 1332 1
		 1354 1176 1 1353 1354 1 1292 1352 1 1354 1293 1 1338 1336 1 1335 1337 1 1333 1355 1
		 1355 1211 1 1356 1332 1 1212 1356 1 1355 1319 1 1322 1356 1 1351 1319 1 1322 1353 1
		 1334 1330 1 1329 1331 1 1326 1357 1 1325 1358 1 1357 1358 1 1328 1359 1 1327 1360 1
		 1359 1360 1 1329 1361 1 1358 1361 1 1330 1362 1 1362 1359 1 1331 1363 1 1335 1364 1
		 1363 1364 1 1336 1365 1 1334 1366 1 1365 1366 1 1337 1367 1 1339 1368 1 1367 1368 1
		 1340 1369 1 1338 1370 1 1369 1370 1 1341 1371 1 1368 1371 1 1342 1372 1 1372 1369 1
		 1343 1373 1 1373 1357 1 1344 1374 1 1360 1374 1 1345 1375 1 1375 1373 1 1346 1376 1
		 1374 1376 1 1347 1377 1 1377 1375 1 1348 1378 1 1376 1378 1 1349 1379 1 1379 1377 1
		 1350 1380 1 1378 1380 1 1371 1379 1 1380 1372 1 1370 1365 1 1364 1367 1 1366 1362 1
		 1361 1363 1 1381 1300 1 1382 1302 1 1383 1301 1 1381 1382 1 1382 1383 1 1298 1381 1
		 1383 1299 1 1306 1384 1 1384 1385 1 1385 1307 1 1308 1386 1 1386 1384 1 1384 1309 1
		 1310 1385 1 1386 1311 1 1324 1385 1 1386 1323 1 1390 1395 1 1387 1388 1 1388 1396 1
		 1389 1390 1 1394 1397 1 1391 1392 1 1392 1398 1 1393 1394 1 1395 1387 1 1396 1389 1
		 1395 1396 1 1397 1391 1 1398 1393 1 1397 1398 1 1400 1399 1 1399 1389 1 1396 1400 1
		 1402 1401 1 1401 1398 1 1392 1402 1 1403 1400 1 1388 1403 1 1401 1404 1 1404 1393 1
		 1406 1390 1 1389 1405 1 1405 1406 1 1391 1407 1 1407 1408 1 1408 1392 1 1410 1406 1
		 1405 1409 1 1409 1410 1 1407 1411 1 1411 1412 1 1412 1408 1 1399 1413 1 1413 1405 1
		 1414 1402 1 1408 1414 1 1387 1415 1 1415 1416 1 1416 1388 1 1418 1394 1 1393 1417 1
		 1417 1418 1 1416 1419 1 1419 1403 1 1404 1420 1 1420 1417 1 1416 1421 1 1421 1422 1
		 1422 1419 1 1424 1417 1 1420 1423 1 1423 1424 1 1413 1425 1 1425 1409 1 1426 1414 1
		 1412 1426 1 1415 1427 1 1427 1421 1 1428 1418 1 1424 1428 1 1425 1429 1 1429 1410 1
		 1430 1426 1 1411 1430 1 1431 1422 1 1427 1431 1 1423 1432 1 1432 1428 1 1431 1433 1;
	setAttr ".ed[2822:2987]" 1433 1434 1 1434 1422 1 1436 1432 1 1423 1435 1 1435 1436 1
		 1433 1437 1 1437 1438 1 1438 1434 1 1440 1436 1 1435 1439 1 1439 1440 1 1438 1362 1
		 1366 1434 1 1435 1363 1 1361 1439 1 1365 1422 1 1423 1364 1 1438 1441 1 1441 1359 1
		 1358 1442 1 1442 1439 1 1437 1443 1 1443 1441 1 1442 1444 1 1444 1440 1 1441 1445 1
		 1445 1360 1 1357 1446 1 1446 1442 1 1443 1447 1 1447 1445 1 1448 1444 1 1446 1448 1
		 1445 1449 1 1449 1374 1 1450 1446 1 1373 1450 1 1447 1451 1 1451 1449 1 1450 1452 1
		 1452 1448 1 1449 1425 1 1426 1450 1 1451 1429 1 1430 1452 1 1370 1419 1 1420 1367 1
		 1369 1403 1 1404 1368 1 1372 1400 1 1401 1371 1 1380 1399 1 1402 1379 1 1378 1413 1
		 1414 1377 1 1376 1425 1 1426 1375 1 1454 1443 1 1437 1453 1 1453 1454 1 1444 1455 1
		 1455 1456 1 1456 1440 1 1457 1447 1 1454 1457 1 1448 1458 1 1458 1455 1 1395 1459 1
		 1459 1460 1 1460 1387 1 1462 1397 1 1394 1461 1 1461 1462 1 1390 1463 1 1463 1459 1
		 1464 1391 1 1462 1464 1 1463 1465 1 1465 1466 1 1466 1459 1 1468 1464 1 1462 1467 1
		 1467 1468 1 1469 1460 1 1466 1469 1 1461 1470 1 1470 1467 1 1453 1471 1 1466 1472 1
		 1470 1474 1 1455 1475 1 1465 1473 1 1458 1476 1 1460 1427 1 1428 1461 1 1471 1469 1
		 1472 1454 1 1473 1457 1 1471 1472 1 1472 1473 1 1474 1456 1 1475 1467 1 1476 1468 1
		 1474 1475 1 1475 1476 1 1469 1431 1 1432 1470 1 1471 1433 1 1436 1474 1 1410 1463 1
		 1464 1411 1 1429 1465 1 1468 1430 1 1451 1473 1 1476 1452 1 1478 1477 1 1477 820 1
		 819 1478 1 1477 1479 1 1479 821 1 1480 1478 1 822 1480 1 1479 1481 1 1481 823 1 1482 1480 1
		 824 1482 1 1481 1483 1 1483 825 1 1484 1482 1 826 1484 1 1483 1485 1 1485 827 1 828 1486 1
		 1486 1484 1 1485 1487 1 1487 829 1 830 1488 1 1488 1486 1 1489 831 1 1487 1489 1
		 1490 1488 1 832 1490 1 1489 1491 1 1491 833 1 834 1492 1 1492 1490 1 1491 1492 1
		 1494 1493 1 1493 1515 1 1492 1516 1 1495 1494 1 1491 1517 1 1497 1496 1 1496 1493 1
		 1494 1497 1 1498 1497 1 1495 1498 1 1500 1499 1 1499 1496 1 1497 1500 1 1501 1500 1
		 1498 1501 1 1478 1502 1 1500 1503 1 1501 1504 1 1502 1499 1;
	setAttr ".ed[2988:3153]" 1503 1477 1 1504 1479 1 1502 1503 1 1503 1504 1 1505 1502 1
		 1480 1505 1 1504 1506 1 1506 1481 1 1482 1507 1 1507 1505 1 1508 1483 1 1506 1508 1
		 1484 1509 1 1509 1507 1 1508 1510 1 1510 1485 1 1511 1509 1 1486 1511 1 1510 1512 1
		 1512 1487 1 1507 1499 1 1501 1508 1 1509 1496 1 1498 1510 1 1511 1493 1 1495 1512 1
		 1488 1513 1 1513 1511 1 1514 1489 1 1512 1514 1 1515 1490 1 1516 1494 1 1517 1495 1
		 1515 1516 1 1516 1517 1 1515 1513 1 1514 1517 1 1518 439 1 1519 441 1 1520 443 1
		 1521 447 1 1522 451 1 1523 455 1 1524 459 1 1525 463 1 1526 467 1 1527 469 1 1528 465 1
		 1529 461 1 1530 457 1 1531 453 1 1532 449 1 1533 445 1 1518 1519 1 1519 1520 1 1520 1521 1
		 1521 1522 1 1522 1523 1 1523 1524 1 1524 1525 1 1525 1526 1 1526 1527 1 1527 1528 1
		 1528 1529 1 1529 1530 1 1530 1531 1 1531 1532 1 1532 1533 1 1533 1518 1 471 1534 1
		 470 1535 1 1534 1535 1 475 1536 1 1536 1534 1 502 1537 1 1537 1536 1 566 1538 1 1538 1537 1
		 562 1539 1 1539 1538 1 558 1540 1 1540 1539 1 554 1541 1 1541 1540 1 550 1542 1 1542 1541 1
		 546 1543 1 1543 1542 1 542 1544 1 1544 1659 1 538 1545 1 1545 1544 1 534 1546 1 1546 1545 1
		 531 1547 1 1547 1546 1 524 1548 1 1548 1547 1 517 1549 1 1549 1548 1 504 1550 1 1550 1549 1
		 1535 1550 1 473 1551 1 472 1552 1 1551 1552 1 507 1553 1 1553 1551 1 516 1554 1 1554 1553 1
		 523 1555 1 1554 1555 1 530 1556 1 1555 1556 1 533 1557 1 1557 1556 1 537 1558 1 1558 1557 1
		 541 1559 1 1559 1558 1 545 1560 1 1560 1674 1 549 1561 1 1561 1560 1 553 1562 1 1562 1561 1
		 557 1563 1 1563 1562 1 561 1564 1 1564 1563 1 565 1565 1 1565 1564 1 503 1566 1 1566 1565 1
		 476 1567 1 1567 1566 1 1552 1567 1 1534 1568 1 1535 1569 1 1568 1569 1 1536 1570 1
		 1570 1568 1 1537 1571 1 1571 1570 1 1538 1572 1 1572 1571 1 1539 1573 1 1573 1572 1
		 1540 1574 1 1574 1573 1 1541 1575 1 1575 1574 1 1542 1576 1 1576 1575 1 1543 1577 1
		 1577 1576 1 1544 1578 1 1578 1658 1 1545 1579 1 1579 1578 1 1546 1580 1 1580 1579 1
		 1547 1581 1 1581 1580 1 1548 1582 1 1582 1581 1;
	setAttr ".ed[3154:3319]" 1549 1583 1 1583 1582 1 1550 1584 1 1584 1583 1 1569 1584 1
		 1551 1585 1 1552 1586 1 1585 1586 1 1553 1587 1 1587 1585 1 1554 1588 1 1588 1587 1
		 1555 1589 1 1588 1589 1 1556 1590 1 1589 1590 1 1557 1591 1 1591 1590 1 1558 1592 1
		 1592 1591 1 1559 1593 1 1593 1592 1 1560 1594 1 1594 1673 1 1561 1595 1 1595 1594 1
		 1562 1596 1 1596 1595 1 1563 1597 1 1597 1596 1 1564 1598 1 1598 1597 1 1565 1599 1
		 1599 1598 1 1566 1600 1 1600 1599 1 1567 1601 1 1601 1600 1 1586 1601 1 1568 1602 1
		 1569 1603 1 1602 1603 1 1570 1604 1 1604 1602 1 1571 1605 1 1605 1604 1 1572 1606 1
		 1606 1605 1 1573 1607 1 1607 1606 1 1574 1608 1 1608 1607 1 1575 1609 1 1609 1608 1
		 1576 1610 1 1610 1609 1 1577 1611 1 1611 1610 1 1578 1612 1 1612 1657 1 1579 1613 1
		 1613 1612 1 1580 1614 1 1614 1613 1 1581 1615 1 1615 1614 1 1582 1616 1 1616 1615 1
		 1583 1617 1 1617 1616 1 1584 1618 1 1618 1617 1 1603 1618 1 1585 1619 1 1586 1620 1
		 1619 1620 1 1587 1621 1 1621 1619 1 1588 1622 1 1622 1621 1 1589 1623 1 1622 1623 1
		 1590 1624 1 1623 1624 1 1591 1625 1 1625 1624 1 1592 1626 1 1626 1625 1 1593 1627 1
		 1627 1626 1 1594 1628 1 1628 1672 1 1595 1629 1 1629 1628 1 1596 1630 1 1630 1629 1
		 1597 1631 1 1631 1630 1 1598 1632 1 1632 1631 1 1599 1633 1 1633 1632 1 1600 1634 1
		 1634 1633 1 1601 1635 1 1635 1634 1 1620 1635 1 1617 1637 1 1622 1640 1 1618 1636 1
		 1621 1639 1 1616 1638 1 1623 1641 1 1636 1644 1 1637 1643 1 1638 1642 1 1602 1636 1
		 1636 1637 1 1637 1638 1 1638 1614 1 1639 1647 1 1640 1646 1 1641 1645 1 1620 1639 1
		 1639 1640 1 1640 1641 1 1641 1625 1 1642 1648 1 1643 1649 1 1644 1650 1 1613 1642 1
		 1642 1643 1 1643 1644 1 1644 1604 1 1645 1651 1 1646 1652 1 1647 1653 1 1626 1645 1
		 1645 1646 1 1646 1647 1 1647 1635 1 1648 1656 1 1649 1655 1 1650 1654 1 1612 1648 1
		 1648 1649 1 1649 1650 1 1650 1605 1 1651 1671 1 1652 1670 1 1653 1669 1 1627 1651 1
		 1651 1652 1 1652 1653 1 1653 1634 1 1654 1608 1 1655 1609 1 1656 1610 1 1657 1611 1
		 1658 1577 1 1659 1543 1 1660 546 1 1661 543 1 1662 601 1 1663 603 1 1664 652 1;
	setAttr ".ed[3320:3384]" 1665 668 1 1666 670 1 1667 672 1 1668 682 1 1654 1655 1
		 1655 1656 1 1656 1657 1 1657 1658 1 1658 1659 1 1659 1660 1 1660 1661 1 1661 1662 1
		 1662 1663 1 1663 1664 1 1664 1665 1 1665 1666 1 1666 1667 1 1667 1668 1 1669 1631 1
		 1670 1630 1 1671 1629 1 1672 1627 1 1673 1593 1 1674 1559 1 1675 541 1 1676 544 1
		 1677 600 1 1678 602 1 1679 1184 1 1680 669 1 1681 671 1 1682 673 1 1683 685 1 1669 1670 1
		 1670 1671 1 1671 1672 1 1672 1673 1 1673 1674 1 1674 1675 1 1675 1676 1 1676 1677 1
		 1677 1678 1 1678 1679 1 1679 1680 1 1680 1681 1 1681 1682 1 1682 1683 1 1654 1606 1
		 1669 1633 1 1685 1684 1 1684 570 1 593 1685 1 1687 1686 1 1686 592 1 569 1687 1 1684 1688 1
		 1688 568 1 1688 1687 1 1686 594 1 595 1685 1 572 1687 1 1688 571 1 1684 573 1 596 1686 1
		 1685 597 1;
	setAttr -s 1698 -ch 6770 ".fc";
	setAttr ".fc[0:499]" -type "polyFaces" 
		f 4 0 1 2 -4
		mu 0 4 0 1 2 3
		f 4 -5 5 6 -8
		mu 0 4 4 5 6 7
		f 4 -7 8 9 -11
		mu 0 4 7 6 8 9
		f 4 -12 12 13 -15
		mu 0 4 10 1854 12 13
		f 4 -14 15 16 -18
		mu 0 4 13 12 14 15
		f 4 -19 19 20 -22
		mu 0 4 16 17 18 19
		f 4 -21 22 23 -25
		mu 0 4 19 18 20 21
		f 4 -26 26 3 -28
		mu 0 4 22 23 0 3
		f 4 -24 28 25 -30
		mu 0 4 21 20 23 22
		f 4 -17 30 18 -32
		mu 0 4 15 14 17 16
		f 4 -10 32 11 -34
		mu 0 4 9 8 11 1825
		f 4 -2 34 4 35
		mu 0 4 2 1 5 4
		f 4 -1 36 37 -39
		mu 0 4 1 0 24 25
		f 4 -6 39 40 -42
		mu 0 4 6 5 26 27
		f 4 -9 41 42 -44
		mu 0 4 8 6 27 28
		f 4 -13 44 45 -47
		mu 0 4 12 1854 1853 30
		f 4 -16 46 47 -49
		mu 0 4 14 12 30 31
		f 4 -20 49 50 -52
		mu 0 4 18 17 32 33
		f 4 -23 51 52 -54
		mu 0 4 20 18 33 34
		f 4 -27 54 55 -37
		mu 0 4 0 23 35 24
		f 4 -29 53 56 -55
		mu 0 4 23 20 34 35
		f 4 -31 48 57 -50
		mu 0 4 17 14 31 32
		f 4 -33 43 58 -45
		mu 0 4 11 8 28 29
		f 4 -35 38 59 -40
		mu 0 4 5 1 25 26
		f 4 -38 60 61 -63
		mu 0 4 25 24 36 37
		f 4 -41 63 64 -66
		mu 0 4 27 26 38 39
		f 4 -43 65 66 -68
		mu 0 4 28 27 39 40
		f 4 -46 68 69 -71
		mu 0 4 30 1853 1851 42
		f 4 -48 70 71 -73
		mu 0 4 31 30 42 43
		f 4 -51 73 74 -76
		mu 0 4 33 32 44 45
		f 4 -53 75 76 -78
		mu 0 4 34 33 45 46
		f 4 -56 78 79 -61
		mu 0 4 24 35 47 36
		f 4 -57 77 80 -79
		mu 0 4 35 34 46 47
		f 4 -58 72 81 -74
		mu 0 4 32 31 43 44
		f 4 -59 67 82 -69
		mu 0 4 29 28 40 41
		f 4 -60 62 83 -64
		mu 0 4 26 25 37 38
		f 4 -62 84 85 -87
		mu 0 4 37 36 1742 1752
		f 4 -84 86 87 -89
		mu 0 4 38 37 1752 1751
		f 4 -65 88 89 -91
		mu 0 4 39 38 1751 1750
		f 4 -67 90 91 -93
		mu 0 4 40 39 1750 1749
		f 4 -83 92 93 -95
		mu 0 4 41 40 1749 1748
		f 4 -70 94 95 -97
		mu 0 4 42 1851 1852 1747
		f 4 -72 96 97 -99
		mu 0 4 43 42 1747 1746
		f 4 -82 98 99 -101
		mu 0 4 44 43 1746 1745
		f 4 -75 100 101 -103
		mu 0 4 45 44 1745 1744
		f 4 -77 102 103 -105
		mu 0 4 46 45 1744 1743
		f 4 -81 104 105 -107
		mu 0 4 47 46 1743 1741
		f 4 -80 106 107 -85
		mu 0 4 36 47 1741 1742
		f 4 108 109 110 111
		mu 0 4 60 61 62 63
		f 4 112 113 114 -116
		mu 0 4 64 65 66 67
		f 4 116 117 118 -114
		mu 0 4 65 68 69 66
		f 4 119 120 121 122
		mu 0 4 70 71 72 73
		f 4 123 124 125 -122
		mu 0 4 1850 74 75 1831
		f 4 -125 126 -112 127
		mu 0 4 75 74 60 63
		f 4 128 129 130 -132
		mu 0 4 76 77 78 1849
		f 4 132 133 -135 135
		mu 0 4 80 81 82 83
		f 4 -130 136 137 138
		mu 0 4 78 77 84 85
		f 4 -138 139 -133 140
		mu 0 4 85 84 81 80
		f 4 -142 -143 143 -145
		mu 0 4 86 87 88 89
		f 4 -146 -147 -144 -148
		mu 0 4 90 91 89 88
		f 4 -136 -149 149 -141
		mu 0 4 80 83 92 85
		f 4 -131 -139 -150 -151
		mu 0 4 79 1922 85 92
		f 4 151 -120 152 -118
		mu 0 4 68 71 70 69
		f 4 153 -129 -155 145
		mu 0 4 90 77 76 91
		f 4 -137 -154 147 155
		mu 0 4 84 77 90 88
		f 4 -140 -156 142 -157
		mu 0 4 81 84 88 87
		f 4 -134 156 141 -158
		mu 0 4 82 81 87 86
		f 4 -110 158 115 159
		mu 0 4 62 61 64 67
		f 4 134 160 -109 161
		mu 0 4 83 82 61 60
		f 4 -159 -161 157 162
		mu 0 4 64 61 82 86
		f 4 163 -113 -163 144
		mu 0 4 89 65 64 86
		f 4 164 -117 -164 146
		mu 0 4 91 68 65 89
		f 4 154 -166 -152 -165
		mu 0 4 91 76 71 68
		f 4 -121 165 131 166
		mu 0 4 72 71 76 1849
		f 4 167 -124 -167 150
		mu 0 4 92 74 1850 79
		f 4 -127 -168 148 -162
		mu 0 4 60 74 92 83
		f 4 168 169 170 171
		mu 0 4 93 94 95 96
		f 4 172 -169 173 174
		mu 0 4 1842 94 93 98
		f 4 175 176 -175 177
		mu 0 4 99 100 97 1848
		f 4 178 179 -176 180
		mu 0 4 101 102 100 99
		f 4 181 182 -184 184
		mu 0 4 103 1843 105 106
		f 4 -186 186 -182 187
		mu 0 4 107 108 104 1846
		f 4 188 189 190 -172
		mu 0 4 96 109 110 93
		f 4 191 -181 192 193
		mu 0 4 111 101 99 112
		f 4 -190 194 195 196
		mu 0 4 110 109 113 1847
		f 4 -196 197 -194 198
		mu 0 4 114 1845 111 112
		f 4 -197 199 -174 -191
		mu 0 4 110 1847 98 93
		f 4 -193 -178 -200 -199
		mu 0 4 112 99 1848 114
		f 4 -201 -188 201 -203
		mu 0 4 115 107 1846 1844
		f 4 -204 -205 -202 -185
		mu 0 4 106 117 116 103
		f 4 -171 205 185 206
		mu 0 4 96 95 108 107
		f 4 207 -189 -207 200
		mu 0 4 115 109 96 107
		f 4 -195 -208 202 208
		mu 0 4 113 109 115 1844
		f 4 -198 -209 204 -210
		mu 0 4 111 1845 116 117
		f 4 -211 -192 209 203
		mu 0 4 106 101 111 117
		f 4 183 211 -179 210
		mu 0 4 106 105 102 101
		f 4 -170 212 213 214
		mu 0 4 95 94 118 119
		f 4 -206 -215 215 216
		mu 0 4 108 95 119 120
		f 4 -187 -217 217 -219
		mu 0 4 104 108 120 121
		f 4 -183 218 219 -221
		mu 0 4 105 1843 1833 122
		f 4 -212 220 221 -223
		mu 0 4 102 105 122 123
		f 4 -180 222 223 -225
		mu 0 4 100 102 123 124
		f 4 -177 224 225 -227
		mu 0 4 97 100 124 125
		f 4 -213 -173 226 227
		mu 0 4 118 94 1842 1832
		f 4 228 229 230 231
		mu 0 4 126 127 128 129
		f 4 232 233 234 -236
		mu 0 4 130 131 1834 133
		f 4 236 237 238 -234
		mu 0 4 1838 134 135 132
		f 4 239 240 241 242
		mu 0 4 136 137 138 139
		f 4 243 244 245 -241
		mu 0 4 137 140 1829 138
		f 4 246 -229 247 -245
		mu 0 4 1836 127 126 141
		f 4 248 -250 250 251
		mu 0 4 142 143 144 145
		f 4 252 253 254 255
		mu 0 4 146 147 148 1841
		f 4 -257 257 -253 258
		mu 0 4 150 151 147 146
		f 4 -255 259 -252 260
		mu 0 4 149 1840 142 145
		f 4 -251 -262 262 -261
		mu 0 4 145 144 152 149
		f 4 -259 -256 -263 -264
		mu 0 4 150 146 1841 1835
		f 4 -265 -266 266 -268
		mu 0 4 153 154 155 1837
		f 4 -269 -270 -271 -267
		mu 0 4 1839 157 158 156
		f 4 271 -243 272 -238
		mu 0 4 134 136 139 135
		f 4 -258 -274 269 -275
		mu 0 4 147 151 158 157
		f 4 -254 274 268 275
		mu 0 4 148 147 157 1839
		f 4 -260 -276 265 -277
		mu 0 4 142 1840 155 154
		f 4 -278 -249 276 264
		mu 0 4 153 143 142 154
		f 4 -231 278 235 279
		mu 0 4 129 128 130 133
		f 4 -230 280 249 281
		mu 0 4 128 127 144 143
		f 4 -279 -282 277 282
		mu 0 4 130 128 143 153
		f 4 283 -233 -283 267
		mu 0 4 1837 131 130 153
		f 4 284 -237 -284 270
		mu 0 4 158 134 1838 156
		f 4 -286 -272 -285 273
		mu 0 4 151 136 134 158
		f 4 286 -240 285 256
		mu 0 4 150 137 136 151
		f 4 287 -244 -287 263
		mu 0 4 1835 140 137 150
		f 4 -281 -247 -288 261
		mu 0 4 144 127 1836 152
		f 4 288 -239 289 -220
		mu 0 4 1833 132 135 122
		f 4 -289 -218 290 -235
		mu 0 4 1834 121 120 133
		f 4 291 -228 292 293
		mu 0 4 159 118 1832 1830
		f 4 -293 -226 294 295
		mu 0 4 160 125 124 161
		f 4 -126 296 -294 297
		mu 0 4 1831 75 159 1830
		f 4 298 299 -301 -160
		mu 0 4 67 162 163 62
		f 4 301 302 -214 -292
		mu 0 4 159 164 119 118
		f 4 303 -216 -303 304
		mu 0 4 165 120 119 164
		f 4 -305 -306 306 307
		mu 0 4 165 164 166 167
		f 4 -309 309 310 -307
		mu 0 4 166 163 168 167
		f 4 311 312 -310 -300
		mu 0 4 162 169 168 163
		f 4 313 -291 -304 314
		mu 0 4 170 133 120 165
		f 4 315 -280 -314 316
		mu 0 4 171 129 133 170
		f 4 317 -232 -316 318
		mu 0 4 172 126 129 171
		f 4 319 -248 -318 320
		mu 0 4 173 141 126 172
		f 4 321 -246 -320 322
		mu 0 4 174 138 1829 1828
		f 4 323 -242 -322 324
		mu 0 4 175 139 138 174
		f 4 325 -273 -324 326
		mu 0 4 176 135 139 175
		f 4 -222 327 328 -330
		mu 0 4 123 122 177 178
		f 4 -224 329 330 -295
		mu 0 4 124 123 178 161
		f 4 331 -328 -290 -326
		mu 0 4 176 177 122 135
		f 3 332 -298 333
		mu 0 3 179 73 160
		f 4 334 335 -334 -296
		mu 0 4 161 180 179 160
		f 4 336 -123 -333 337
		mu 0 4 181 70 73 179
		f 4 338 -153 -337 339
		mu 0 4 182 69 70 181
		f 4 340 341 -335 -331
		mu 0 4 178 183 180 161
		f 4 -341 -329 342 -344
		mu 0 4 183 178 177 184
		f 4 -338 -336 344 345
		mu 0 4 181 179 180 185
		f 4 346 -340 -346 347
		mu 0 4 186 182 181 185
		f 4 -345 -342 348 349
		mu 0 4 185 180 183 187
		f 4 350 -348 -350 351
		mu 0 4 188 186 185 187
		f 4 -349 343 352 353
		mu 0 4 187 183 184 189
		f 3 -343 -332 354
		mu 0 3 184 177 176
		f 4 355 356 -355 -327
		mu 0 4 175 190 184 176
		f 4 357 358 -353 -357
		mu 0 4 190 191 189 184
		f 4 359 360 -356 -325
		mu 0 4 174 192 190 175
		f 4 361 -358 -361 362
		mu 0 4 193 191 190 192
		f 4 363 364 -360 -323
		mu 0 4 1828 1827 192 174
		f 4 365 -363 -365 366
		mu 0 4 1826 193 192 1827
		f 4 367 368 -364 -321
		mu 0 4 172 196 194 173
		f 4 -367 -369 369 370
		mu 0 4 195 194 196 197
		f 4 371 372 -368 -319
		mu 0 4 171 198 196 172
		f 3 373 -315 -308
		mu 0 3 167 170 165
		f 4 -372 -317 -374 374
		mu 0 4 198 171 170 167
		f 4 375 376 -375 -311
		mu 0 4 168 199 198 167
		f 4 -370 -373 -377 377
		mu 0 4 197 196 198 199
		f 4 -376 -313 378 379
		mu 0 4 199 168 169 200
		f 4 380 381 -378 -380
		mu 0 4 200 201 197 199
		f 3 -128 382 -297
		mu 0 3 75 63 159
		f 4 383 -111 300 308
		mu 0 4 166 63 62 163
		f 4 -384 305 -302 -383
		mu 0 4 63 166 164 159
		f 4 384 385 386 21
		mu 0 4 19 202 203 16
		f 4 -354 387 -389 -352
		mu 0 4 187 189 204 188
		f 4 389 390 31 -387
		mu 0 4 203 205 15 16
		f 4 391 -393 -388 -359
		mu 0 4 191 206 204 189
		f 4 393 -299 -115 394
		mu 0 4 207 162 67 66
		f 4 -119 -339 395 -395
		mu 0 4 66 69 182 207
		f 4 -396 396 397 398
		mu 0 4 207 182 208 209
		f 4 -400 400 -385 24
		mu 0 4 21 210 202 19
		f 4 27 401 -398 -403
		mu 0 4 22 3 209 208
		f 4 403 404 -394 -399
		mu 0 4 209 211 162 207
		f 4 -404 -402 -3 405
		mu 0 4 211 209 3 2
		f 4 406 407 -409 7
		mu 0 4 7 212 213 4
		f 4 409 -407 10 410
		mu 0 4 214 212 7 9
		f 4 411 412 -392 -362
		mu 0 4 193 215 206 191
		f 4 -391 413 414 17
		mu 0 4 15 205 216 13
		f 4 415 -412 -366 416
		mu 0 4 1823 215 193 1826
		f 4 -417 -371 -382 417
		mu 0 4 217 195 197 201
		f 4 -415 418 419 14
		mu 0 4 13 216 218 10
		f 4 -420 420 -411 33
		mu 0 4 1825 1824 214 9
		f 4 -401 -422 -351 -423
		mu 0 4 202 210 186 188
		f 4 -386 422 388 423
		mu 0 4 203 202 188 204
		f 4 424 -390 -424 392
		mu 0 4 206 205 203 204
		f 4 -414 -425 -413 425
		mu 0 4 216 205 206 215
		f 4 -419 -426 -416 426
		mu 0 4 218 216 215 1823
		f 4 -421 -427 -418 -428
		mu 0 4 214 1824 217 201
		f 4 -429 -410 427 -381
		mu 0 4 200 212 214 201
		f 4 -408 428 -379 -430
		mu 0 4 213 212 200 169
		f 4 -406 -36 408 430
		mu 0 4 211 2 4 213
		f 4 -312 -405 -431 429
		mu 0 4 169 162 211 213
		f 4 421 431 -397 -347
		mu 0 4 186 210 208 182
		f 4 402 -432 399 29
		mu 0 4 22 208 210 21
		f 4 432 433 434 435
		mu 0 4 219 220 221 222
		f 4 -435 436 437 -439
		mu 0 4 222 221 223 224
		f 4 439 440 441 -443
		mu 0 4 225 226 227 1822
		f 4 -442 443 444 -446
		mu 0 4 228 1791 229 230
		f 4 446 447 448 -450
		mu 0 4 231 232 233 234
		f 4 -449 450 451 -453
		mu 0 4 234 233 235 236
		f 4 453 454 455 -457
		mu 0 4 237 238 239 240
		f 4 -456 457 458 -460
		mu 0 4 240 239 241 242
		f 4 -452 460 -454 -462
		mu 0 4 236 235 238 237
		f 4 -445 462 -447 -464
		mu 0 4 230 229 232 231
		f 4 -438 464 -440 -466
		mu 0 4 224 223 226 225
		f 4 466 -433 467 -459
		mu 0 4 241 220 219 242
		f 4 438 468 -470 -471
		mu 0 4 222 224 243 244
		f 4 465 471 -473 -469
		mu 0 4 224 225 245 243
		f 4 445 473 -475 -476
		mu 0 4 228 230 246 247
		f 4 463 476 -478 -474
		mu 0 4 230 231 248 246
		f 4 452 478 -480 -481
		mu 0 4 234 236 249 250
		f 4 461 481 -483 -479
		mu 0 4 236 237 251 249
		f 4 459 483 -485 -486
		mu 0 4 240 242 252 253
		f 4 -436 470 486 -488
		mu 0 4 219 222 244 254
		f 4 456 485 -489 -482
		mu 0 4 237 240 253 251
		f 4 449 480 -490 -477
		mu 0 4 231 234 250 248
		f 4 442 475 -491 -472
		mu 0 4 225 1822 1819 245
		f 4 -468 487 491 -484
		mu 0 4 242 219 254 252
		f 4 492 493 -495 -496
		mu 0 4 255 256 1740 1737
		f 4 496 497 -499 -494
		mu 0 4 256 259 1731 1740
		f 4 499 500 -502 -503
		mu 0 4 1820 262 1739 1732
		f 4 503 504 -506 -501
		mu 0 4 262 265 1733 1739
		f 4 506 507 -509 -510
		mu 0 4 267 268 1738 1734
		f 4 510 511 -513 -508
		mu 0 4 268 271 1735 1738
		f 4 513 514 -516 -517
		mu 0 4 273 274 1730 1736
		f 4 517 495 518 -520
		mu 0 4 277 255 1737 1729
		f 4 520 516 -522 -512
		mu 0 4 271 273 1736 1735
		f 4 522 509 -524 -505
		mu 0 4 265 267 1734 1733
		f 4 524 502 -526 -498
		mu 0 4 259 261 1821 1731
		f 4 526 519 527 -515
		mu 0 4 274 277 1729 1730
		f 4 -492 528 -527 -530
		mu 0 4 252 254 277 274
		f 4 -487 530 -518 -529
		mu 0 4 254 244 255 277
		f 4 469 531 -493 -531
		mu 0 4 244 243 256 255
		f 4 472 532 -497 -532
		mu 0 4 243 245 259 256
		f 4 490 533 -525 -533
		mu 0 4 245 1819 261 259
		f 4 474 534 -500 -534
		mu 0 4 247 246 262 1820
		f 4 477 535 -504 -535
		mu 0 4 246 248 265 262
		f 4 489 536 -523 -536
		mu 0 4 248 250 267 265
		f 4 479 537 -507 -537
		mu 0 4 250 249 268 267
		f 4 482 538 -511 -538
		mu 0 4 249 251 271 268
		f 4 488 539 -521 -539
		mu 0 4 251 253 273 271
		f 4 484 529 -514 -540
		mu 0 4 253 252 274 273
		f 4 540 541 542 543
		mu 0 4 279 280 281 282
		f 4 544 545 546 -544
		mu 0 4 282 283 284 279
		f 4 547 548 549 -546
		mu 0 4 283 285 286 284
		f 4 550 551 552 -549
		mu 0 4 285 287 288 286
		f 4 553 554 555 -557
		mu 0 4 1817 290 291 1818
		f 4 -555 557 -559 559
		mu 0 4 291 290 293 294
		f 4 560 561 562 -553
		mu 0 4 288 295 296 286
		f 4 563 564 -541 565
		mu 0 4 297 298 280 279
		f 4 566 567 -564 568
		mu 0 4 299 300 298 297
		f 4 -562 569 -567 570
		mu 0 4 296 295 300 299
		f 4 -566 -547 571 -569
		mu 0 4 297 279 284 299
		f 4 -563 -571 -572 -550
		mu 0 4 286 296 299 284
		f 4 -573 -574 574 -560
		mu 0 4 294 301 302 291
		f 4 -576 -556 -575 -577
		mu 0 4 1921 1818 291 302
		f 4 577 -552 578 556
		mu 0 4 292 288 287 289
		f 4 579 -561 -578 575
		mu 0 4 303 295 288 292
		f 4 -570 -580 576 -581
		mu 0 4 300 295 303 302
		f 4 -568 580 573 -582
		mu 0 4 298 300 302 301
		f 4 -565 581 572 -583
		mu 0 4 280 298 301 294
		f 4 583 -542 582 558
		mu 0 4 293 281 280 294
		f 4 -585 -584 585 586
		mu 0 4 304 281 293 305
		f 4 -543 584 587 588
		mu 0 4 282 281 304 306
		f 4 589 -545 -589 590
		mu 0 4 307 283 282 306
		f 4 591 -548 -590 592
		mu 0 4 308 285 283 307
		f 4 593 -551 -592 594
		mu 0 4 309 287 285 308
		f 4 -579 -594 595 596
		mu 0 4 289 287 309 310
		f 4 597 -554 -597 598
		mu 0 4 311 290 1817 1798
		f 4 -558 -598 599 -586
		mu 0 4 293 290 311 305
		f 4 600 601 602 603
		mu 0 4 312 313 314 315
		f 4 604 -606 606 607
		mu 0 4 1810 317 318 319
		f 4 608 609 -608 610
		mu 0 4 320 321 316 1800
		f 4 611 612 613 614
		mu 0 4 322 323 324 325
		f 4 615 616 -612 617
		mu 0 4 326 1812 323 322
		f 4 -603 618 -616 619
		mu 0 4 315 314 327 1802
		f 4 620 621 622 -624
		mu 0 4 328 329 330 331
		f 4 624 -626 626 627
		mu 0 4 332 333 334 335
		f 4 628 629 -628 630
		mu 0 4 336 1814 332 335
		f 4 -622 631 -629 632
		mu 0 4 330 329 337 1816
		f 4 -634 -635 -636 636
		mu 0 4 1815 339 340 341
		f 4 -638 -639 -637 -640
		mu 0 4 342 343 338 1811
		f 4 -623 -633 640 -642
		mu 0 4 331 330 1816 1813
		f 4 -627 -643 -641 -631
		mu 0 4 335 334 344 336
		f 4 605 643 -601 644
		mu 0 4 318 317 313 312
		f 4 645 -621 -647 634
		mu 0 4 339 329 328 340
		f 4 -632 -646 633 -648
		mu 0 4 337 329 339 1815
		f 4 -630 647 638 -649
		mu 0 4 332 1814 338 343
		f 4 -650 -625 648 637
		mu 0 4 342 333 332 343
		f 4 -614 650 -609 651
		mu 0 4 325 324 321 320
		f 4 -644 652 646 -654
		mu 0 4 313 317 340 328
		f 4 -602 653 623 654
		mu 0 4 314 313 328 331
		f 4 -619 -655 641 -656
		mu 0 4 327 314 331 1813
		f 4 -617 655 642 -657
		mu 0 4 323 1812 344 334
		f 4 -613 656 625 657
		mu 0 4 324 323 334 333
		f 4 -651 -658 649 -659
		mu 0 4 321 324 333 342
		f 4 -610 658 639 -660
		mu 0 4 316 321 342 1811
		f 4 -653 -605 659 635
		mu 0 4 340 317 1810 341
		f 4 660 661 662 663
		mu 0 4 345 346 347 348
		f 4 664 665 666 -663
		mu 0 4 347 349 1807 348
		f 4 667 668 669 -666
		mu 0 4 1804 351 352 350
		f 4 670 671 672 -669
		mu 0 4 351 353 354 352
		f 4 673 674 675 -677
		mu 0 4 355 356 1809 358
		f 4 677 -679 679 -675
		mu 0 4 1803 359 360 357
		f 4 680 -664 681 682
		mu 0 4 361 345 348 362
		f 4 683 684 -683 685
		mu 0 4 363 1805 361 362
		f 4 -673 686 -688 688
		mu 0 4 352 354 365 366
		f 4 687 689 -684 690
		mu 0 4 366 365 364 1808
		f 4 -692 -693 693 -680
		mu 0 4 360 367 368 357
		f 4 -695 -676 -694 -696
		mu 0 4 369 358 1809 1806
		f 4 -682 -667 696 -686
		mu 0 4 362 348 1807 363
		f 4 -691 -697 -670 -689
		mu 0 4 366 1808 350 352
		f 4 697 676 698 -672
		mu 0 4 353 355 358 354
		f 4 -687 -699 694 -700
		mu 0 4 365 354 358 369
		f 4 -690 699 695 -701
		mu 0 4 364 365 369 1806
		f 4 -685 700 692 -702
		mu 0 4 361 1805 368 367
		f 4 -703 -681 701 691
		mu 0 4 360 345 361 367
		f 4 678 703 -661 702
		mu 0 4 360 359 346 345
		f 4 -704 704 705 -707
		mu 0 4 346 359 370 371
		f 4 -662 706 707 708
		mu 0 4 347 346 371 372
		f 4 709 -665 -709 710
		mu 0 4 373 349 347 372
		f 4 711 -668 -710 712
		mu 0 4 374 351 1804 1801
		f 4 713 -671 -712 714
		mu 0 4 375 353 351 374
		f 4 715 -698 -714 716
		mu 0 4 376 355 353 375
		f 4 717 -674 -716 718
		mu 0 4 377 356 355 376
		f 4 -705 -678 -718 719
		mu 0 4 370 359 1803 1797
		f 4 720 -618 721 -713
		mu 0 4 1801 326 322 374
		f 4 -721 -711 722 -620
		mu 0 4 1802 373 372 315
		f 4 723 724 725 -607
		mu 0 4 318 378 1799 319
		f 4 -726 726 727 -611
		mu 0 4 1800 379 380 320
		f 4 -599 728 -725 729
		mu 0 4 311 1798 1799 378
		f 4 730 -588 -732 732
		mu 0 4 381 306 304 382
		f 4 733 -724 -645 734
		mu 0 4 383 378 318 312
		f 4 735 736 -735 -604
		mu 0 4 315 384 383 312
		f 4 -737 737 738 -740
		mu 0 4 383 384 385 386
		f 4 -741 -739 741 742
		mu 0 4 382 386 385 387
		f 4 743 -733 -743 744
		mu 0 4 388 381 382 387
		f 4 745 746 -736 -723
		mu 0 4 372 389 384 315
		f 4 747 748 -746 -708
		mu 0 4 371 390 389 372
		f 4 749 750 -748 -706
		mu 0 4 370 391 390 371
		f 4 751 752 -750 -720
		mu 0 4 1797 1796 391 370
		f 4 753 754 -752 -719
		mu 0 4 376 393 392 377
		f 4 755 756 -754 -717
		mu 0 4 375 394 393 376
		f 4 757 758 -756 -715
		mu 0 4 374 395 394 375
		f 4 -652 -728 759 -761
		mu 0 4 325 320 380 396
		f 4 -615 760 761 -763
		mu 0 4 322 325 396 397
		f 4 763 -758 -722 762
		mu 0 4 397 395 374 322
		f 3 764 765 -729
		mu 0 3 310 398 379
		f 4 766 -727 -766 767
		mu 0 4 399 380 379 398
		f 4 768 769 -765 -596
		mu 0 4 309 400 398 310
		f 4 770 771 -769 -595
		mu 0 4 308 401 400 309
		f 4 772 -760 -767 773
		mu 0 4 402 396 380 399
		f 4 -773 774 775 -762
		mu 0 4 396 402 403 397
		f 4 -770 776 777 -768
		mu 0 4 398 400 404 399
		f 4 778 779 -777 -772
		mu 0 4 401 405 404 400
		f 4 -778 780 781 -774
		mu 0 4 399 404 406 402
		f 4 782 783 -781 -780
		mu 0 4 405 407 406 404
		f 4 -782 784 785 -775
		mu 0 4 402 406 408 403
		f 3 -776 786 -764
		mu 0 3 397 403 395
		f 4 787 -759 -787 788
		mu 0 4 409 394 395 403
		f 4 789 -789 -786 790
		mu 0 4 410 409 403 408
		f 4 791 -757 -788 792
		mu 0 4 411 393 394 409
		f 4 793 794 -793 -790
		mu 0 4 410 412 411 409
		f 4 795 -755 -792 796
		mu 0 4 1795 392 393 411
		f 4 797 798 -797 -795
		mu 0 4 412 414 1795 411
		f 4 799 -753 -796 800
		mu 0 4 415 391 1796 413
		f 4 -799 801 802 -801
		mu 0 4 413 1794 416 415
		f 4 803 -751 -800 804
		mu 0 4 417 390 391 415
		f 3 -738 -747 805
		mu 0 3 385 384 389
		f 4 -804 806 -806 -749
		mu 0 4 390 417 385 389
		f 4 807 -742 -807 808
		mu 0 4 418 387 385 417
		f 4 -803 809 -809 -805
		mu 0 4 415 416 418 417
		f 4 -808 810 811 -745
		mu 0 4 387 418 419 388
		f 4 812 -811 -810 813
		mu 0 4 420 419 418 416
		f 3 -600 -730 814
		mu 0 3 305 311 378
		f 4 815 740 731 -587
		mu 0 4 305 386 382 304
		f 4 -816 -815 -734 739
		mu 0 4 386 305 378 383
		f 4 816 817 818 819
		mu 0 4 407 421 422 423
		f 4 -785 -784 -820 820
		mu 0 4 408 406 407 423
		f 4 821 822 823 -819
		mu 0 4 422 424 425 423
		f 4 824 -791 -821 -824
		mu 0 4 425 410 408 423
		f 4 825 826 -591 -731
		mu 0 4 381 426 307 306
		f 4 -593 -827 827 -771
		mu 0 4 308 307 426 401
		f 4 -828 828 829 830
		mu 0 4 401 426 427 428
		f 4 -832 832 -817 -783
		mu 0 4 405 429 421 407
		f 4 -458 -834 -830 834
		mu 0 4 241 239 428 427
		f 4 835 -829 -826 836
		mu 0 4 430 427 426 381
		f 4 -836 837 -467 -835
		mu 0 4 427 430 220 241
		f 4 838 839 -841 -812
		mu 0 4 419 431 432 388
		f 4 841 -839 -813 842
		mu 0 4 433 431 419 420
		f 4 843 -794 -825 844
		mu 0 4 434 412 410 425
		f 4 -823 845 846 -845
		mu 0 4 425 424 435 434
		f 4 847 848 -798 -844
		mu 0 4 434 436 414 412
		f 4 -849 849 -814 -802
		mu 0 4 1794 1793 420 416
		f 4 -847 850 851 -848
		mu 0 4 434 435 437 436
		f 4 -852 852 -843 -850
		mu 0 4 1793 1792 433 420
		f 4 -833 -854 -461 -855
		mu 0 4 421 429 238 235
		f 4 -818 854 -451 855
		mu 0 4 422 421 235 233
		f 4 856 -822 -856 -448
		mu 0 4 232 424 422 233
		f 4 -846 -857 -463 857
		mu 0 4 435 424 232 229
		f 4 -851 -858 -444 858
		mu 0 4 437 435 229 1791
		f 4 -853 -859 -441 -860
		mu 0 4 433 1792 227 226
		f 4 -861 -842 859 -465
		mu 0 4 223 431 433 226
		f 4 -840 860 -437 -862
		mu 0 4 432 431 223 221
		f 4 -838 862 861 -434
		mu 0 4 220 430 432 221
		f 4 -744 840 -863 -837
		mu 0 4 381 388 432 430
		f 4 831 -779 -831 863
		mu 0 4 429 405 401 428
		f 4 833 -455 853 -864
		mu 0 4 428 239 238 429
		f 4 864 3041 3026 867
		mu 0 4 438 439 440 441
		f 4 868 -3027 3042 3027
		mu 0 4 442 441 440 443
		f 4 3056 -865 872 873
		mu 0 4 444 439 438 445
		f 4 3043 3028 876 -3028
		mu 0 4 443 446 447 442
		f 4 3055 -874 878 879
		mu 0 4 448 444 445 449
		f 4 3044 3029 882 -3029
		mu 0 4 446 450 451 447
		f 4 883 884 3054 -880
		mu 0 4 449 452 453 448
		f 4 886 -3030 3045 3030
		mu 0 4 454 451 450 455
		f 4 889 890 3053 -885
		mu 0 4 452 456 457 453
		f 4 892 -3031 3046 3031
		mu 0 4 458 454 455 459
		f 4 895 896 3052 -891
		mu 0 4 456 460 461 457
		f 4 898 -3032 3047 3032
		mu 0 4 462 458 459 463
		f 4 3051 -897 902 903
		mu 0 4 464 461 460 465
		f 4 3048 3033 906 -3033
		mu 0 4 463 466 467 462
		f 4 3050 -904 908 909
		mu 0 4 468 464 465 469
		f 4 3049 -910 911 -3034
		mu 0 4 466 468 469 467
		f 4 912 913 -883 914
		mu 0 4 470 471 447 451
		f 4 915 916 -879 917
		mu 0 4 472 473 449 445
		f 4 918 -915 919 920
		mu 0 4 474 470 451 475
		f 4 921 922 923 -917
		mu 0 4 473 476 477 449
		f 4 924 925 -920 -887
		mu 0 4 454 478 475 451
		f 4 926 -884 -924 927
		mu 0 4 479 452 449 477
		f 4 -893 928 929 -925
		mu 0 4 454 458 480 478
		f 4 -890 -927 930 931
		mu 0 4 456 452 479 481
		f 4 932 933 -912 934
		mu 0 4 482 483 467 469
		f 4 935 -935 -909 936
		mu 0 4 484 482 469 465
		f 4 937 938 -907 -934
		mu 0 4 483 485 462 467
		f 4 939 -937 -903 940
		mu 0 4 486 484 465 460
		f 4 941 -929 -899 -939
		mu 0 4 485 480 458 462
		f 4 942 -941 -896 -932
		mu 0 4 481 486 460 456
		f 4 943 944 945 946
		mu 0 4 487 488 489 485
		f 4 947 948 949 950
		mu 0 4 490 491 486 492
		f 4 951 952 -933 953
		mu 0 4 493 494 483 482
		f 4 954 -954 -936 955
		mu 0 4 495 493 482 484
		f 4 -938 -953 956 -947
		mu 0 4 485 483 494 487
		f 4 -940 -949 957 -956
		mu 0 4 484 486 491 495
		f 4 960 -959 -928 961
		mu 0 4 496 497 479 477
		f 4 962 963 -926 -960
		mu 0 4 498 499 475 478
		f 4 964 -942 -946 965
		mu 0 4 500 480 485 489
		f 4 966 967 -950 -943
		mu 0 4 481 501 492 486
		f 4 -965 968 959 -930
		mu 0 4 480 500 498 478
		f 4 -967 -931 958 969
		mu 0 4 501 481 479 497
		f 4 970 -921 -964 971
		mu 0 4 502 474 475 499
		f 4 972 973 -962 -923
		mu 0 4 476 503 496 477
		f 4 974 975 976 -914
		mu 0 4 471 504 505 447
		f 4 977 -918 978 979
		mu 0 4 506 472 445 507
		f 4 -977 980 981 -877
		mu 0 4 447 505 508 442
		f 4 -979 -873 982 983
		mu 0 4 507 445 438 509
		f 4 984 -869 -982 985
		mu 0 4 510 441 442 508
		f 4 -985 986 -983 -868
		mu 0 4 441 510 509 438
		f 4 987 988 989 -987
		mu 0 4 510 511 512 509
		f 4 -988 -986 990 991
		mu 0 4 511 510 508 513
		f 4 992 -984 -990 993
		mu 0 4 514 507 509 512
		f 4 994 995 -991 -981
		mu 0 4 505 515 513 508
		f 4 996 997 -980 -993
		mu 0 4 514 516 506 507
		f 4 998 -995 -976 999
		mu 0 4 517 515 505 504
		f 4 -989 1000 1002 -1002
		mu 0 4 512 511 518 519
		f 4 -992 1003 1004 -1001
		mu 0 4 511 513 520 518
		f 4 -994 1001 1006 -1006
		mu 0 4 514 512 519 521
		f 4 -996 1007 1008 -1004
		mu 0 4 513 515 522 520
		f 4 -997 1005 1010 -1010
		mu 0 4 516 514 521 523
		f 4 -999 1011 1012 -1008
		mu 0 4 515 517 524 522
		f 4 -1003 1013 1015 -1015
		mu 0 4 519 518 525 526
		f 4 -1005 1016 1017 -1014
		mu 0 4 518 520 527 525
		f 4 -1007 1014 1019 -1019
		mu 0 4 521 519 526 528
		f 4 -1009 1020 1021 -1017
		mu 0 4 520 522 529 527
		f 4 -1011 1018 1023 -1023
		mu 0 4 523 521 528 530
		f 4 -1013 1024 1025 -1021
		mu 0 4 522 524 531 529
		f 4 1026 -1024 1027 1028
		mu 0 4 532 530 528 533
		f 4 1029 1030 1031 -1026
		mu 0 4 531 534 535 529
		f 4 1032 -1029 1033 1034
		mu 0 4 536 532 533 537
		f 4 1035 1036 1037 -1031
		mu 0 4 534 538 539 535;
	setAttr ".fc[500:999]"
		f 4 1038 1039 1040 -1035
		mu 0 4 537 540 541 536
		f 4 1041 -1037 1042 1043
		mu 0 4 542 539 538 543
		f 4 1044 3359 3345 1046
		mu 0 4 544 545 546 547
		f 4 1047 3330 3316 -1044
		mu 0 4 543 548 549 542
		f 4 1050 1051 1052 -1047
		mu 0 4 547 550 551 544
		f 4 1053 -1049 1054 1055
		mu 0 4 552 553 554 555
		f 4 1056 -1052 1057 1058
		mu 0 4 556 551 550 557
		f 4 1059 1060 1061 -1056
		mu 0 4 555 558 559 552
		f 4 1062 -1059 1063 1064
		mu 0 4 560 556 557 561
		f 4 1065 1066 1067 -1061
		mu 0 4 558 562 563 559
		f 4 1068 -1065 1069 1070
		mu 0 4 564 560 561 565
		f 4 1071 1072 1073 -1067
		mu 0 4 562 566 567 563
		f 4 1074 -1071 1075 1076
		mu 0 4 568 564 565 569
		f 4 1077 1078 1079 -1073
		mu 0 4 566 570 571 567
		f 4 1080 -974 1081 -1077
		mu 0 4 569 496 503 568
		f 4 1082 -1079 1083 -972
		mu 0 4 499 571 570 502
		f 4 1084 1085 1086 -1016
		mu 0 4 525 572 573 526
		f 4 -1085 -1018 1087 1088
		mu 0 4 572 525 527 574
		f 4 -1090 1091 1093 -1093
		mu 0 4 575 576 577 578
		f 4 -1091 1094 1095 -1092
		mu 0 4 576 579 580 577
		f 4 -1094 1096 1098 -1098
		mu 0 4 578 577 1789 1785
		f 4 -1096 1099 1100 -1097
		mu 0 4 577 580 583 1789
		f 4 -1099 1101 1103 -1103
		mu 0 4 582 1790 1704 585
		f 4 -1101 1104 1105 -1102
		mu 0 4 581 1788 586 584
		f 4 -1104 1106 1108 -1108
		mu 0 4 585 1704 1703 588
		f 4 -1106 1109 1110 -1107
		mu 0 4 584 586 589 587
		f 4 -1109 1111 1113 -1113
		mu 0 4 588 1703 1702 591
		f 4 -1111 1114 1115 -1112
		mu 0 4 587 589 592 590
		f 4 -1114 1116 1118 -1118
		mu 0 4 591 1702 1701 594
		f 4 -1116 1119 1120 -1117
		mu 0 4 590 592 595 593
		f 4 -1032 1121 -1088 -1022
		mu 0 4 529 535 574 527
		f 4 -1028 -1020 -1087 1122
		mu 0 4 533 528 526 573
		f 4 1123 1124 -1034 -1123
		mu 0 4 573 596 537 533
		f 4 1125 -1122 -1038 1126
		mu 0 4 597 574 535 539
		f 4 1127 1128 -1039 -1125
		mu 0 4 596 598 540 537
		f 4 1129 -1127 -1042 1130
		mu 0 4 599 597 539 542
		f 4 3360 3346 1138 -3346
		mu 0 4 546 600 601 547
		f 4 -1131 -3317 3331 -1140
		mu 0 4 599 542 549 602
		f 4 3361 3347 1142 -3347
		mu 0 4 600 603 604 601
		f 4 -1136 1139 3332 -1144
		mu 0 4 605 599 602 606
		f 4 -1139 1145 1146 -1051
		mu 0 4 547 601 607 550
		f 4 -1141 -1054 1148 -1148
		mu 0 4 608 553 552 609
		f 4 -1147 1151 1152 -1058
		mu 0 4 550 607 610 557
		f 4 -1149 -1062 1154 -1154
		mu 0 4 609 552 559 611
		f 4 -1150 1155 1156 -1152
		mu 0 4 607 612 613 610
		f 4 -1151 1153 1158 -1158
		mu 0 4 614 609 611 615
		f 4 -1153 1159 1160 -1064
		mu 0 4 557 610 616 561
		f 4 -1155 -1068 1162 -1162
		mu 0 4 611 559 563 617
		f 4 -1157 1163 1164 -1160
		mu 0 4 610 613 618 616
		f 4 -1159 1161 1166 -1166
		mu 0 4 615 611 617 619
		f 4 -1161 1167 1168 -1070
		mu 0 4 561 616 620 565
		f 4 -1163 -1074 1170 -1170
		mu 0 4 617 563 567 621
		f 4 -1165 1171 1172 -1168
		mu 0 4 616 618 622 620
		f 4 -1167 1169 1174 -1174
		mu 0 4 619 617 621 623
		f 4 -1169 1175 1176 -1185
		mu 0 4 565 620 624 625
		f 4 -1171 1183 1178 -1178
		mu 0 4 621 567 626 627
		f 4 -1173 1179 1180 -1176
		mu 0 4 620 622 628 624
		f 4 -1175 1177 1182 -1182
		mu 0 4 623 621 627 629
		f 4 -1080 1185 1186 -1184
		mu 0 4 567 571 630 626
		f 4 -1076 1184 1187 1188
		mu 0 4 569 565 625 631
		f 4 -1083 -963 1189 -1186
		mu 0 4 571 499 498 630
		f 4 -1081 -1189 1190 -961
		mu 0 4 496 569 631 497
		f 4 -1177 1191 1193 -1193
		mu 0 4 625 624 632 633
		f 4 -1179 1194 1196 -1196
		mu 0 4 627 626 634 635
		f 4 -1181 1197 1198 -1192
		mu 0 4 624 628 636 632
		f 4 -1183 1195 1200 -1200
		mu 0 4 629 627 635 637
		f 4 -1187 1201 1202 -1195
		mu 0 4 626 630 638 634
		f 4 -1188 1192 1204 -1204
		mu 0 4 631 625 633 639
		f 4 -1190 -969 1205 -1202
		mu 0 4 630 498 500 638
		f 4 -1191 1203 1206 -970
		mu 0 4 497 631 639 501
		f 4 -1194 1207 1209 -1209
		mu 0 4 633 632 640 641
		f 4 -1197 1210 1212 -1212
		mu 0 4 635 634 642 643
		f 4 -1199 1213 1214 -1208
		mu 0 4 632 636 644 640
		f 4 -1201 1211 1216 -1216
		mu 0 4 637 635 643 645
		f 4 -1203 1217 1218 -1211
		mu 0 4 634 638 646 642
		f 4 -1205 1208 1220 -1220
		mu 0 4 639 633 641 647
		f 4 -1206 -966 1221 -1218
		mu 0 4 638 500 489 646
		f 4 -1207 1219 1222 -968
		mu 0 4 501 639 647 492
		f 4 -1210 1223 1225 -1225
		mu 0 4 641 640 648 649
		f 4 -1213 1226 1228 -1228
		mu 0 4 643 642 650 651
		f 4 -1215 1229 1230 -1224
		mu 0 4 640 644 652 648
		f 4 -1217 1227 1232 -1232
		mu 0 4 645 643 651 653
		f 4 -1219 1233 1234 -1227
		mu 0 4 642 646 654 650
		f 4 -1221 1224 1236 -1236
		mu 0 4 647 641 649 655
		f 4 -1222 -945 1237 -1234
		mu 0 4 646 489 488 654
		f 4 -1223 1235 1238 -951
		mu 0 4 492 647 655 490
		f 4 1149 -1146 -1143 1239
		mu 0 4 612 607 601 604
		f 4 1150 1240 -1145 1147
		mu 0 4 609 614 656 608
		f 4 1243 1244 -1137 -1242
		mu 0 4 657 658 659 605
		f 4 1245 -1243 -1134 1246
		mu 0 4 660 661 662 663
		f 4 -1095 -1133 -1245 1247
		mu 0 4 580 579 659 658
		f 4 1092 1248 -1247 -1132
		mu 0 4 575 578 660 663
		f 4 -1244 1249 1250 1251
		mu 0 4 658 657 664 1787
		f 4 -1246 1252 1253 1254
		mu 0 4 661 660 1786 1783
		f 4 -1252 1255 -1100 -1248
		mu 0 4 658 1787 583 580
		f 4 -1253 -1249 1097 1256
		mu 0 4 1786 660 578 1785
		f 4 -1251 1257 1259 -1259
		mu 0 4 665 1784 668 669
		f 4 -1254 1260 1262 -1262
		mu 0 4 667 666 670 671
		f 4 -1256 1258 1263 -1105
		mu 0 4 1788 665 669 586
		f 4 -1257 1102 1264 -1261
		mu 0 4 666 582 585 670
		f 4 -1260 1265 1267 -1267
		mu 0 4 669 668 672 673
		f 4 -1263 1268 1270 -1270
		mu 0 4 671 670 674 675
		f 4 -1264 1266 1271 -1110
		mu 0 4 586 669 673 589
		f 4 -1265 1107 1272 -1269
		mu 0 4 670 585 588 674
		f 4 -1258 1273 3335 -1276
		mu 0 4 668 1784 676 677
		f 4 3364 3350 -1279 -3350
		mu 0 4 678 679 680 681
		f 4 -1266 1275 3336 -1280
		mu 0 4 672 668 677 682
		f 4 3365 3351 -1283 -3351
		mu 0 4 679 683 684 680
		f 4 1285 -1277 -1284 1286
		mu 0 4 685 686 687 688
		f 4 1287 1288 -1285 1278
		mu 0 4 680 689 690 681
		f 4 -1286 1289 1290 -1281
		mu 0 4 686 685 691 692
		f 4 -1288 1282 1291 1292
		mu 0 4 689 680 684 693
		f 4 -1272 1293 1294 -1115
		mu 0 4 589 673 694 592
		f 4 -1273 1112 1295 1296
		mu 0 4 674 588 591 695
		f 4 -1268 1297 1298 -1294
		mu 0 4 673 672 696 694
		f 4 -1271 -1297 1299 1300
		mu 0 4 675 674 695 697
		f 4 3337 3323 -1298 1279
		mu 0 4 682 698 696 672
		f 4 1303 -3352 3366 3352
		mu 0 4 699 684 683 700
		f 4 1305 1306 -1302 -1291
		mu 0 4 691 701 702 692
		f 4 1307 -1292 -1304 1308
		mu 0 4 703 693 684 699
		f 4 1309 1310 -1120 -1295
		mu 0 4 694 704 595 592
		f 4 1311 -1296 1117 1312
		mu 0 4 705 695 591 594
		f 4 1313 1314 -1310 -1299
		mu 0 4 696 706 704 694
		f 4 1315 -1300 -1312 1316
		mu 0 4 707 697 695 705
		f 4 1317 -1314 -3324 -1303
		mu 0 4 702 706 696 698
		f 4 -3353 -1305 -1316 1318
		mu 0 4 699 700 697 707
		f 4 1319 1320 -1318 -1307
		mu 0 4 701 708 706 702
		f 4 1321 -1309 -1319 1322
		mu 0 4 709 703 699 707
		f 4 -1119 1323 1325 -1325
		mu 0 4 594 1701 1700 711
		f 4 -1121 1326 1327 -1324
		mu 0 4 593 595 712 710
		f 4 -1311 1328 1329 -1327
		mu 0 4 595 704 713 712
		f 4 -1313 1324 1331 -1331
		mu 0 4 705 594 711 714
		f 4 -1315 -1321 1332 -1329
		mu 0 4 704 706 708 713
		f 4 -1317 1330 1333 -1323
		mu 0 4 707 705 714 709
		f 4 1335 1402 1401 -1377
		mu 0 4 715 716 717 718
		f 4 1405 1404 1336 1381
		mu 0 4 719 720 721 722
		f 4 -1335 1338 1340 -1340
		mu 0 4 723 724 725 726
		f 4 -1337 1341 1343 -1343
		mu 0 4 722 721 727 728
		f 4 -1341 1344 1396 -1346
		mu 0 4 726 725 729 730
		f 4 1399 1398 1349 -1398
		mu 0 4 731 732 733 1919
		f 4 -1347 1350 1352 -1352
		mu 0 4 735 1920 737 738
		f 4 -1350 1353 1355 -1355
		mu 0 4 734 1859 739 740
		f 4 -1353 1356 1390 -1358
		mu 0 4 738 737 741 742
		f 4 1393 1392 1361 -1392
		mu 0 4 743 744 745 746
		f 4 -1359 1362 1364 -1364
		mu 0 4 747 748 749 750
		f 4 -1362 1365 1367 -1367
		mu 0 4 746 745 751 752
		f 4 -1365 1368 1384 -1370
		mu 0 4 750 749 753 754
		f 4 1387 1386 1373 -1386
		mu 0 4 755 756 757 758
		f 4 -1371 1374 1376 -1376
		mu 0 4 759 760 715 718
		f 4 -1374 1377 1379 -1379
		mu 0 4 758 757 761 762
		f 4 -1385 1382 1370 -1384
		mu 0 4 754 753 760 759
		f 4 -1368 1371 -1388 -1373
		mu 0 4 752 751 756 755
		f 4 -1391 1388 1358 -1390
		mu 0 4 742 741 748 747
		f 4 -1356 1359 -1394 -1361
		mu 0 4 740 739 744 743
		f 4 -1397 1394 1346 -1396
		mu 0 4 730 729 736 1858
		f 4 -1344 1347 -1400 -1349
		mu 0 4 728 727 732 731
		f 4 -1403 1400 1334 1380
		mu 0 4 717 716 724 723
		f 4 1337 -1406 1403 -1380
		mu 0 4 761 720 719 762
		f 4 -1336 1406 1408 -1408
		mu 0 4 716 715 763 764
		f 4 -1339 1409 1411 -1411
		mu 0 4 725 724 765 766
		f 4 1342 1413 -1415 -1413
		mu 0 4 722 728 767 768
		f 4 -1345 1410 1416 -1416
		mu 0 4 729 725 766 769
		f 4 1348 1417 -1419 -1414
		mu 0 4 728 731 770 767
		f 4 -1351 1419 1421 -1421
		mu 0 4 737 1920 1918 772
		f 4 1354 1423 -1425 -1423
		mu 0 4 734 740 773 774
		f 4 -1357 1420 1426 -1426
		mu 0 4 741 737 772 775
		f 4 1360 1427 -1429 -1424
		mu 0 4 740 743 776 773
		f 4 -1363 1429 1431 -1431
		mu 0 4 749 748 777 778
		f 4 1366 1433 -1435 -1433
		mu 0 4 746 752 779 780
		f 4 -1369 1430 1436 -1436
		mu 0 4 753 749 778 781
		f 4 1372 1437 -1439 -1434
		mu 0 4 752 755 782 779
		f 4 -1375 1439 1440 -1407
		mu 0 4 715 760 783 763
		f 4 1378 1442 -1444 -1442
		mu 0 4 758 762 784 785
		f 4 -1382 1412 1445 -1445
		mu 0 4 719 722 768 786
		f 4 -1383 1435 1446 -1440
		mu 0 4 760 753 781 783
		f 4 1385 1441 -1448 -1438
		mu 0 4 755 758 785 782
		f 4 -1389 1425 1448 -1430
		mu 0 4 748 741 775 777
		f 4 1391 1432 -1450 -1428
		mu 0 4 743 746 780 776
		f 4 -1395 1415 1450 -1420
		mu 0 4 736 729 769 771
		f 4 1397 1422 -1452 -1418
		mu 0 4 731 1919 1917 770
		f 4 -1401 1407 1452 -1410
		mu 0 4 724 716 764 765
		f 4 -1404 1444 1453 -1443
		mu 0 4 762 719 786 784
		f 4 -1409 1454 1514 -1456
		mu 0 4 764 763 787 788
		f 4 -1412 1457 1516 -1459
		mu 0 4 766 765 789 790
		f 4 1540 1529 -1463 -1529
		mu 0 4 791 792 1727 1721
		f 4 -1417 1458 1517 -1464
		mu 0 4 769 766 790 795
		f 4 1541 1530 -1467 -1530
		mu 0 4 792 796 1709 1727
		f 4 -1422 1467 1519 -1469
		mu 0 4 772 1918 798 799
		f 4 1543 1532 -1473 -1532
		mu 0 4 800 801 1725 1710
		f 4 -1427 1468 1520 -1474
		mu 0 4 775 772 799 804
		f 4 1544 1533 -1477 -1533
		mu 0 4 801 805 1713 1725
		f 4 -1432 1477 1522 -1479
		mu 0 4 778 777 807 808
		f 4 1546 1535 -1483 -1535
		mu 0 4 809 810 1723 1714
		f 4 -1437 1478 1523 -1484
		mu 0 4 781 778 808 813
		f 4 1547 1536 -1487 -1536
		mu 0 4 810 814 1717 1723
		f 4 -1441 1487 1525 -1455
		mu 0 4 763 783 816 787
		f 4 1549 1526 -1492 -1538
		mu 0 4 817 818 1706 1718
		f 4 1539 1528 1493 -1528
		mu 0 4 821 791 1721 1705
		f 4 -1447 1483 1524 -1488
		mu 0 4 783 781 813 816
		f 4 1548 1537 -1496 -1537
		mu 0 4 814 817 1718 1717
		f 4 -1449 1473 1521 -1478
		mu 0 4 777 775 804 807
		f 4 1545 1534 -1498 -1534
		mu 0 4 805 809 1714 1713
		f 4 -1451 1463 1518 -1468
		mu 0 4 771 769 795 1915
		f 4 1542 1531 -1500 -1531
		mu 0 4 796 1913 1914 1709
		f 4 -1453 1455 1515 -1458
		mu 0 4 765 764 788 789
		f 4 1538 1527 1501 -1527
		mu 0 4 818 821 1705 1706
		f 4 -1515 1502 1456 -1504
		mu 0 4 788 787 1722 1707
		f 4 -1516 1503 1500 -1505
		mu 0 4 789 788 1707 1708
		f 4 -1517 1504 1459 -1506
		mu 0 4 790 789 1708 1728
		f 4 -1518 1505 1464 -1507
		mu 0 4 795 790 1728 1711
		f 4 -1519 1506 1498 -1508
		mu 0 4 1915 795 1711 1712
		f 4 -1520 1507 1469 -1509
		mu 0 4 799 798 1916 1726
		f 4 -1521 1508 1474 -1510
		mu 0 4 804 799 1726 1715
		f 4 -1522 1509 1496 -1511
		mu 0 4 807 804 1715 1716
		f 4 -1523 1510 1479 -1512
		mu 0 4 808 807 1716 1724
		f 4 -1524 1511 1484 -1513
		mu 0 4 813 808 1724 1719
		f 4 -1525 1512 1494 -1514
		mu 0 4 816 813 1719 1720
		f 4 -1526 1513 1488 -1503
		mu 0 4 787 816 1720 1722
		f 4 -1454 1492 -1539 -1491
		mu 0 4 784 786 821 818
		f 4 -1446 1460 -1540 -1493
		mu 0 4 786 768 791 821
		f 4 1414 1461 -1541 -1461
		mu 0 4 768 767 792 791
		f 4 1418 1465 -1542 -1462
		mu 0 4 767 770 796 792
		f 4 1451 1470 -1543 -1466
		mu 0 4 770 1917 1913 796
		f 4 1424 1471 -1544 -1471
		mu 0 4 774 773 801 800
		f 4 1428 1475 -1545 -1472
		mu 0 4 773 776 805 801
		f 4 1449 1480 -1546 -1476
		mu 0 4 776 780 809 805
		f 4 1434 1481 -1547 -1481
		mu 0 4 780 779 810 809
		f 4 1438 1485 -1548 -1482
		mu 0 4 779 782 814 810
		f 4 1447 1489 -1549 -1486
		mu 0 4 782 785 817 814
		f 4 1443 1490 -1550 -1490
		mu 0 4 785 784 818 817
		f 4 -866 1550 1552 -1552
		mu 0 4 835 836 837 838
		f 4 -870 1551 1554 -1554
		mu 0 4 839 835 838 840
		f 4 -872 1555 1556 -1551
		mu 0 4 836 841 842 837
		f 4 -875 1553 1558 -1558
		mu 0 4 843 839 840 844
		f 4 -878 1559 1560 -1556
		mu 0 4 841 845 846 842
		f 4 -881 1557 1562 -1562
		mu 0 4 847 843 844 848
		f 4 -886 1563 1564 -1560
		mu 0 4 845 849 850 846
		f 4 -888 1561 1566 -1566
		mu 0 4 851 847 848 852
		f 4 -892 1567 1568 -1564
		mu 0 4 849 853 854 850
		f 4 -894 1565 1570 -1570
		mu 0 4 855 851 852 856
		f 4 -898 1571 1572 -1568
		mu 0 4 853 857 858 854
		f 4 -900 1569 1574 -1574
		mu 0 4 859 855 856 860
		f 4 -902 1575 1576 -1572
		mu 0 4 857 861 862 858
		f 4 -905 1573 1578 -1578
		mu 0 4 863 859 860 864
		f 4 -908 1579 1580 -1576
		mu 0 4 861 865 866 862
		f 4 -911 1577 1581 -1580
		mu 0 4 865 863 864 866
		f 4 1678 1671 1650 1582
		mu 0 4 867 868 869 870
		f 4 1617 1669 1695 1688
		mu 0 4 871 872 873 874
		f 4 1680 1673 1587 -1673
		mu 0 4 875 876 877 878
		f 4 1696 1689 1590 -1689
		mu 0 4 874 879 880 871
		f 4 1681 1674 1593 -1674
		mu 0 4 876 881 882 877
		f 4 1697 1690 1596 -1690
		mu 0 4 879 883 884 880
		f 4 1651 1683 1676 1610
		mu 0 4 885 886 1909 888
		f 4 1698 1691 1658 -1691
		mu 0 4 883 889 890 884
		f 4 1684 1677 1601 -1677
		mu 0 4 887 891 892 1872
		f 4 1700 1693 1604 -1693
		mu 0 4 893 894 895 1908
		f 4 -1678 1685 -1583 1607
		mu 0 4 892 891 867 870
		f 4 -1694 1701 -1586 1609
		mu 0 4 895 894 897 898
		f 4 1652 1646 1619 -1646
		mu 0 4 899 900 901 902
		f 4 1665 1659 1622 -1659
		mu 0 4 890 903 904 884
		f 4 1624 1655 -1616 1626
		mu 0 4 905 906 907 908
		f 4 1627 1668 -1618 1629
		mu 0 4 909 910 872 871
		f 4 -1647 1653 1647 1635
		mu 0 4 901 900 911 912
		f 4 1639 1667 -1628 1633
		mu 0 4 913 914 910 909
		f 4 -1648 1654 -1625 1631
		mu 0 4 912 911 906 905
		f 4 -1660 1666 -1640 1637
		mu 0 4 904 903 914 913
		f 4 -1626 -1635 1640 -1589
		mu 0 4 915 916 917 918
		f 4 -1630 -1591 1641 -1634
		mu 0 4 909 871 880 913
		f 4 -1621 -1595 -1641 -1631
		mu 0 4 919 920 918 917
		f 4 -1623 -1638 -1642 -1597
		mu 0 4 884 904 913 880
		f 4 -1627 -1607 1642 -1632
		mu 0 4 905 908 921 912
		f 4 -1629 -1639 1643 -1610
		mu 0 4 898 922 923 895
		f 4 -1620 -1636 -1643 -1603
		mu 0 4 1912 1910 912 921
		f 4 -1624 -1605 -1644 -1633
		mu 0 4 924 1908 895 923
		f 4 1682 -1652 1644 -1675
		mu 0 4 881 886 885 882
		f 4 1618 -1653 -1612 1620
		mu 0 4 919 900 899 920
		f 4 -1654 -1619 1630 1636
		mu 0 4 911 900 919 917
		f 4 -1655 -1637 1634 -1649
		mu 0 4 906 911 917 916
		f 4 -1656 1648 1625 -1650
		mu 0 4 907 906 916 915
		f 4 -1672 1679 1672 1614
		mu 0 4 869 868 875 878
		f 4 1613 -1692 1699 1692
		mu 0 4 896 890 889 1907
		f 4 1621 -1666 -1614 1623
		mu 0 4 1911 903 890 896
		f 4 -1667 -1622 1632 -1661
		mu 0 4 914 903 1911 923
		f 4 -1668 1660 1638 -1662
		mu 0 4 910 914 923 922
		f 4 -1669 1661 1628 -1663
		mu 0 4 872 910 922 898
		f 4 1694 -1670 1662 1585
		mu 0 4 897 873 872 898
		f 4 1615 1656 -1679 1670
		mu 0 4 908 907 868 867
		f 4 -1680 -1657 1649 1583
		mu 0 4 875 868 907 915
		f 4 1586 -1681 -1584 1588
		mu 0 4 918 876 875 915
		f 4 1592 -1682 -1587 1594
		mu 0 4 920 881 876 918
		f 4 1611 -1676 -1683 -1593
		mu 0 4 920 899 886 881
		f 4 -1684 1675 1645 1598
		mu 0 4 1909 886 899 902
		f 4 1600 -1685 -1599 1602
		mu 0 4 921 891 887 1912
		f 4 -1686 -1601 1606 -1671
		mu 0 4 867 891 921 908
		f 4 -1688 -1695 1686 1616
		mu 0 4 925 873 897 926
		f 4 -1696 1687 1663 1584
		mu 0 4 874 873 925 927
		f 4 1589 -1697 -1585 1591
		mu 0 4 928 879 874 927
		f 4 1595 -1698 -1590 1597
		mu 0 4 929 883 879 928
		f 4 1664 -1699 -1596 1612
		mu 0 4 930 889 883 929
		f 4 -1700 -1665 1657 1599
		mu 0 4 1907 889 930 931
		f 4 1603 -1701 -1600 1605
		mu 0 4 932 894 893 1869
		f 4 -1702 -1604 1608 -1687
		mu 0 4 897 894 932 926
		f 4 1702 1798 1791 1765
		mu 0 4 933 934 935 936
		f 4 1784 1815 1808 1709
		mu 0 4 937 938 939 940
		f 4 1805 -1703 1710 1711
		mu 0 4 941 934 933 942
		f 4 1821 -1707 1713 1714
		mu 0 4 943 944 945 946
		f 4 1716 1804 -1712 1718
		mu 0 4 947 948 1897 1906
		f 4 1719 1820 -1715 1721
		mu 0 4 949 950 1893 1873
		f 4 1776 1803 -1717 1724
		mu 0 4 951 952 948 947
		f 4 1725 1818 1811 1783
		mu 0 4 953 954 955 956
		f 4 1728 1801 -1723 1730
		mu 0 4 957 958 959 960
		f 4 1731 1817 -1726 1733
		mu 0 4 961 962 954 953
		f 4 -1793 1800 -1729 1735
		mu 0 4 963 964 1898 1904
		f 4 -1809 1816 -1732 1737
		mu 0 4 940 939 1895 1876
		f 4 1772 1766 1739 -1766
		mu 0 4 936 965 966 933
		f 4 1785 1779 1742 -1779
		mu 0 4 967 968 969 970
		f 4 1775 -1725 1744 1745
		mu 0 4 971 951 947 972
		f 4 1788 -1727 1747 1748
		mu 0 4 973 974 975 976
		f 4 -1767 1773 1767 1755
		mu 0 4 966 965 1901 978
		f 4 1759 1787 -1749 1753
		mu 0 4 979 980 973 976
		f 4 -1768 1774 -1746 1751
		mu 0 4 1902 977 971 972
		f 4 -1780 1786 -1760 1757
		mu 0 4 969 968 1899 1903
		f 4 -1756 1760 -1711 -1740
		mu 0 4 966 978 942 933
		f 4 -1753 -1744 -1716 1761
		mu 0 4 1900 982 983 984
		f 4 -1745 -1719 -1761 -1752
		mu 0 4 972 947 1906 1902
		f 4 -1750 -1759 -1762 -1721
		mu 0 4 985 986 981 1894
		f 4 -1741 -1736 1762 -1751
		mu 0 4 987 963 1904 988
		f 4 -1743 -1758 1763 -1737
		mu 0 4 970 969 1903 1896
		f 4 -1747 -1755 -1763 -1731
		mu 0 4 960 990 1905 957
		f 4 -1748 -1733 -1764 -1754
		mu 0 4 976 975 989 979
		f 4 -1792 1799 1792 1705
		mu 0 4 936 935 964 963
		f 4 1738 -1773 -1706 1740
		mu 0 4 987 965 936 963
		f 4 -1774 -1739 1750 1756
		mu 0 4 1901 965 987 988
		f 4 -1775 -1757 1754 -1769
		mu 0 4 971 977 1905 990
		f 4 -1770 -1776 1768 1746
		mu 0 4 960 951 971 990
		f 4 1722 1802 -1777 1769
		mu 0 4 960 959 952 951
		f 4 1706 1814 -1785 1777
		mu 0 4 945 944 938 937
		f 4 1741 -1786 -1708 1743
		mu 0 4 982 968 967 983
		f 4 -1787 -1742 1752 -1781
		mu 0 4 1899 968 982 1900
		f 4 -1788 1780 1758 -1782
		mu 0 4 973 980 981 986
		f 4 -1783 -1789 1781 1749
		mu 0 4 985 974 973 986
		f 4 -1812 1819 -1720 1727
		mu 0 4 956 955 950 949
		f 4 -1799 1790 1703 1771
		mu 0 4 935 934 991 992
		f 4 -1800 -1772 1764 1704
		mu 0 4 964 935 992 993
		f 4 -1801 -1705 1734 -1794
		mu 0 4 1898 964 993 994
		f 4 -1802 1793 1729 -1795
		mu 0 4 959 958 1877 995
		f 4 -1803 1794 1723 -1796
		mu 0 4 952 959 995 996
		f 4 -1804 1795 1770 -1797
		mu 0 4 948 952 996 997
		f 4 -1805 1796 1717 -1798
		mu 0 4 1897 948 997 998
		f 4 -1791 -1806 1797 1712
		mu 0 4 991 934 941 1874
		f 4 -1815 1806 1707 -1808
		mu 0 4 938 944 983 967
		f 4 -1816 1807 1778 1708
		mu 0 4 939 938 967 970
		f 4 -1817 -1709 1736 -1810
		mu 0 4 1895 939 970 1896
		f 4 -1818 1809 1732 -1811
		mu 0 4 954 962 989 975
		f 4 -1819 1810 1726 1789
		mu 0 4 955 954 975 974
		f 4 -1820 -1790 1782 -1813
		mu 0 4 950 955 974 985
		f 4 -1821 1812 1720 -1814
		mu 0 4 1893 950 985 1894
		f 4 -1807 -1822 1813 1715
		mu 0 4 983 944 943 984
		f 4 1822 1918 1911 1890
		mu 0 4 999 1000 1001 1002
		f 4 1909 1935 1928 1829
		mu 0 4 1003 1004 1005 1006
		f 4 1920 1913 1831 -1913
		mu 0 4 1007 1884 1878 1010
		f 4 1936 1929 1834 -1929
		mu 0 4 1005 1881 1012 1006
		f 4 1921 1914 1837 -1914
		mu 0 4 1008 1013 1014 1009
		f 4 1937 1930 1840 -1930
		mu 0 4 1011 1015 1016 1882
		f 4 1923 1916 1843 1891
		mu 0 4 1017 1018 1019 1020
		f 4 1938 1931 1898 -1931
		mu 0 4 1015 1021 1022 1016
		f 4 1924 1917 1849 -1917
		mu 0 4 1018 1883 1868 1019
		f 4 1940 1933 1852 -1933
		mu 0 4 1025 1879 1027 1028
		f 4 1925 -1823 1854 -1918
		mu 0 4 1023 1000 999 1024
		f 4 1941 -1827 1856 -1934
		mu 0 4 1026 1029 1030 1880
		f 4 1895 -1824 1858 1859
		mu 0 4 1031 1032 1033 1034
		f 4 1908 -1830 1861 1862
		mu 0 4 1035 1003 1006 1036
		f 4 1864 1893 1887 1875
		mu 0 4 1037 1038 1887 1040
		f 4 1879 1907 -1863 1869
		mu 0 4 1041 1042 1035 1036
		f 4 -1886 1892 -1865 1871
		mu 0 4 1043 1044 1038 1037
		f 4 -1899 1905 -1868 1873
		mu 0 4 1016 1022 1045 1046
		f 4 -1888 1894 -1860 1866
		mu 0 4 1888 1039 1031 1034
		f 4 1867 1906 -1880 1877
		mu 0 4 1046 1045 1885 1889
		f 4 -1859 -1856 1880 -1867
		mu 0 4 1034 1033 1892 1888
		f 4 -1864 -1879 1881 -1857
		mu 0 4 1030 1048 1049 1880
		f 4 -1872 -1876 -1881 -1851
		mu 0 4 1043 1037 1040 1047
		f 4 -1873 -1853 -1882 -1869
		mu 0 4 1050 1028 1027 1886
		f 4 -1861 -1875 1882 -1833
		mu 0 4 1051 1052 1890 1054
		f 4 -1862 -1835 1883 -1870
		mu 0 4 1036 1006 1012 1041
		f 4 -1866 -1871 -1839 -1883
		mu 0 4 1053 1055 1056 1891
		f 4 -1878 -1884 -1841 -1874
		mu 0 4 1046 1889 1882 1016
		f 4 1922 -1892 1884 -1915
		mu 0 4 1013 1017 1020 1014
		f 4 -1893 -1845 1870 -1887
		mu 0 4 1038 1044 1056 1055
		f 4 -1894 1886 1865 1876
		mu 0 4 1887 1038 1055 1053
		f 4 -1895 -1877 1874 -1889
		mu 0 4 1031 1039 1890 1052
		f 4 -1890 -1896 1888 1860
		mu 0 4 1051 1032 1031 1052
		f 4 -1912 1919 1912 1825
		mu 0 4 1002 1001 1007 1010
		f 4 1939 1932 1846 -1932
		mu 0 4 1021 1025 1028 1022
		f 4 -1906 -1847 1872 -1900
		mu 0 4 1045 1022 1028 1050
		f 4 -1907 1899 1868 -1901
		mu 0 4 1885 1045 1050 1886
		f 4 -1908 1900 1878 -1902
		mu 0 4 1035 1042 1049 1048
		f 4 -1903 -1909 1901 1863
		mu 0 4 1030 1003 1035 1048
		f 4 1826 1934 -1910 1902
		mu 0 4 1030 1029 1004 1003
		f 4 -1919 1910 1823 1896
		mu 0 4 1001 1000 1033 1032
		f 4 -1920 -1897 1889 1824
		mu 0 4 1007 1001 1032 1051
		f 4 1830 -1921 -1825 1832
		mu 0 4 1054 1884 1007 1051
		f 4 1836 -1922 -1831 1838
		mu 0 4 1056 1013 1008 1891
		f 4 -1916 -1923 -1837 1844
		mu 0 4 1044 1017 1013 1056
		f 4 1842 -1924 1915 1885
		mu 0 4 1043 1018 1017 1044
		f 4 1848 -1925 -1843 1850
		mu 0 4 1047 1883 1018 1043
		f 4 -1911 -1926 -1849 1855
		mu 0 4 1033 1000 1023 1892
		f 4 -1935 1926 1827 -1928
		mu 0 4 1004 1029 1057 1058
		f 4 -1936 1927 1903 1828
		mu 0 4 1005 1004 1058 1059
		f 4 1833 -1937 -1829 1835
		mu 0 4 1060 1881 1005 1059
		f 4 1839 -1938 -1834 1841
		mu 0 4 1061 1015 1011 1875
		f 4 1904 -1939 -1840 1847
		mu 0 4 1062 1021 1015 1061
		f 4 1845 -1940 -1905 1897
		mu 0 4 1063 1025 1021 1062
		f 4 1851 -1941 -1846 1853
		mu 0 4 1064 1879 1025 1063
		f 4 -1927 -1942 -1852 1857
		mu 0 4 1057 1029 1026 1867
		f 4 1942 -1838 1943 -1730
		mu 0 4 1877 1009 1014 995
		f 4 1944 -1734 1945 -1842
		mu 0 4 1875 961 953 1061
		f 4 -1943 -1735 1946 -1832
		mu 0 4 1878 994 993 1010
		f 4 -1945 -1836 1947 -1738
		mu 0 4 1876 1060 1059 940
		f 4 1948 -1713 1949 1950
		mu 0 4 1065 991 1874 1871
		f 4 1951 1952 1953 -1714
		mu 0 4 945 1067 1870 946
		f 4 -1950 -1718 1954 1955
		mu 0 4 1066 998 997 1069
		f 4 -1954 1956 1957 -1722
		mu 0 4 1873 1068 1070 949
		f 4 -1602 1958 -1951 1959
		mu 0 4 1872 892 1065 1871
		f 4 -1606 1960 -1953 1961
		mu 0 4 932 1869 1870 1067
		f 4 1966 1967 -1963 -1615
		mu 0 4 878 1071 1072 869
		f 4 1968 -1664 -1966 1969
		mu 0 4 1073 927 925 1074
		f 4 1970 1971 -1704 -1949
		mu 0 4 1065 1075 992 991
		f 4 1972 -1952 -1778 1973
		mu 0 4 1076 1067 945 937
		f 4 1974 -1765 -1972 1975
		mu 0 4 1077 993 992 1075
		f 4 1976 1977 -1974 -1710
		mu 0 4 940 1078 1076 937
		f 4 -1976 -1979 1980 1981
		mu 0 4 1077 1075 1079 1080
		f 4 -1978 1982 1983 -1980
		mu 0 4 1076 1078 1081 1082
		f 4 -1964 1984 1985 -1981
		mu 0 4 1079 1072 1083 1080
		f 4 -1965 -1984 1986 1987
		mu 0 4 1074 1082 1081 1084
		f 4 1988 1989 -1985 -1968
		mu 0 4 1071 1085 1083 1072
		f 4 1990 -1970 -1988 1991
		mu 0 4 1086 1073 1074 1084
		f 4 1992 -1947 -1975 1993
		mu 0 4 1087 1010 993 1077
		f 4 1994 1995 -1977 -1948
		mu 0 4 1059 1088 1078 940
		f 4 1996 -1826 -1993 1997
		mu 0 4 1089 1002 1010 1087
		f 4 1998 1999 -1995 -1904
		mu 0 4 1058 1090 1088 1059
		f 4 2000 -1891 -1997 2001
		mu 0 4 1091 999 1002 1089
		f 4 2002 2003 -1999 -1828
		mu 0 4 1057 1092 1090 1058
		f 4 2004 -1855 -2001 2005
		mu 0 4 1093 1024 999 1091
		f 4 2006 2007 -2003 -1858
		mu 0 4 1867 1865 1092 1057
		f 4 2008 -1850 -2005 2009
		mu 0 4 1095 1019 1868 1866
		f 4 2010 2011 -2007 -1854
		mu 0 4 1063 1096 1094 1064
		f 4 2012 -1844 -2009 2013
		mu 0 4 1097 1020 1019 1095
		f 4 2014 2015 -2011 -1898
		mu 0 4 1062 1098 1096 1063
		f 4 2016 -1885 -2013 2017
		mu 0 4 1099 1014 1020 1097
		f 4 2018 2019 -2015 -1848
		mu 0 4 1061 1100 1098 1062
		f 4 -1724 2020 2022 -2022
		mu 0 4 996 995 1101 1102
		f 4 -1728 -1958 2024 -2024
		mu 0 4 956 949 1070 1103
		f 4 -1771 2021 2025 -1955
		mu 0 4 997 996 1102 1069
		f 4 -1784 2023 2027 -2027
		mu 0 4 953 956 1103 1104
		f 4 2028 -2021 -1944 -2017
		mu 0 4 1099 1101 995 1014
		f 4 2029 -2019 -1946 2026
		mu 0 4 1104 1100 1061 953
		f 3 2030 -1960 2031
		mu 0 3 1105 888 1066
		f 3 2032 2033 -1961
		mu 0 3 931 1106 1068
		f 4 2034 2035 -2032 -1956
		mu 0 4 1069 1107 1105 1066
		f 4 2036 -1957 -2034 2037
		mu 0 4 1108 1070 1068 1106
		f 4 2038 -1611 -2031 2039
		mu 0 4 1109 885 888 1105
		f 4 2040 2041 -2033 -1658
		mu 0 4 930 1110 1106 931
		f 4 2042 -1645 -2039 2043
		mu 0 4 1111 882 885 1109
		f 4 2044 2045 -2041 -1613
		mu 0 4 929 1112 1110 930
		f 4 2046 2047 -2035 -2026
		mu 0 4 1102 1113 1107 1069
		f 4 2048 -2025 -2037 2049
		mu 0 4 1114 1103 1070 1108
		f 4 -2047 -2023 2051 -2051
		mu 0 4 1113 1102 1101 1115
		f 4 -2049 2052 2053 -2028
		mu 0 4 1103 1114 1116 1104
		f 4 -2040 -2036 2054 2055
		mu 0 4 1109 1105 1107 1117
		f 4 -2042 2056 2057 -2038
		mu 0 4 1106 1110 1118 1108
		f 4 2058 -2044 -2056 2059
		mu 0 4 1119 1111 1109 1117
		f 4 2060 2061 -2057 -2046
		mu 0 4 1112 1120 1118 1110
		f 4 -2055 -2048 2062 2063
		mu 0 4 1117 1107 1113 1121
		f 4 -2058 2064 2065 -2050
		mu 0 4 1108 1118 1122 1114
		f 4 2066 -2060 -2064 2067
		mu 0 4 1123 1119 1117 1121
		f 4 2068 2069 -2065 -2062
		mu 0 4 1120 1124 1122 1118
		f 4 -2063 2050 2070 2071
		mu 0 4 1121 1113 1115 1125
		f 4 -2066 2072 2073 -2053
		mu 0 4 1114 1122 1126 1116
		f 3 -2052 -2029 2074
		mu 0 3 1115 1101 1099
		f 3 -2054 2075 -2030
		mu 0 3 1104 1116 1100
		f 4 2076 2077 -2075 -2018
		mu 0 4 1097 1127 1115 1099
		f 4 2078 -2020 -2076 2079
		mu 0 4 1128 1098 1100 1116
		f 4 2080 2081 -2071 -2078
		mu 0 4 1127 1129 1125 1115
		f 4 2082 -2080 -2074 2083
		mu 0 4 1130 1128 1116 1126
		f 4 2084 2085 -2077 -2014
		mu 0 4 1095 1131 1127 1097
		f 4 2086 -2016 -2079 2087
		mu 0 4 1132 1096 1098 1128
		f 4 2088 -2081 -2086 2089
		mu 0 4 1133 1129 1127 1131
		f 4 2090 2091 -2088 -2083
		mu 0 4 1130 1134 1132 1128
		f 4 2092 2093 -2085 -2010
		mu 0 4 1866 1864 1131 1095
		f 4 2094 -2012 -2087 2095
		mu 0 4 1863 1094 1096 1132
		f 4 2096 -2090 -2094 2097
		mu 0 4 1862 1133 1131 1864
		f 4 2098 2099 -2096 -2092
		mu 0 4 1134 1138 1863 1132
		f 4 2100 2101 -2093 -2006
		mu 0 4 1091 1139 1135 1093
		f 4 2102 -2008 -2095 2103
		mu 0 4 1140 1092 1865 1136
		f 4 -2098 -2102 2104 2105
		mu 0 4 1137 1135 1139 1141
		f 4 -2100 2106 2107 -2104
		mu 0 4 1136 1861 1142 1140
		f 4 2108 2109 -2101 -2002
		mu 0 4 1089 1143 1139 1091
		f 4 2110 -2004 -2103 2111
		mu 0 4 1144 1090 1092 1140
		f 3 2112 -1994 -1982
		mu 0 3 1080 1087 1077
		f 3 -1983 -1996 2113
		mu 0 3 1081 1078 1088;
	setAttr ".fc[1000:1499]"
		f 4 -2109 -1998 -2113 2114
		mu 0 4 1143 1089 1087 1080
		f 4 -2111 2115 -2114 -2000
		mu 0 4 1090 1144 1081 1088
		f 4 2116 2117 -2115 -1986
		mu 0 4 1083 1145 1143 1080
		f 4 2118 -1987 -2116 2119
		mu 0 4 1146 1084 1081 1144
		f 4 -2105 -2110 -2118 2120
		mu 0 4 1141 1139 1143 1145
		f 4 -2108 2121 -2120 -2112
		mu 0 4 1140 1142 1146 1144
		f 4 -2117 -1990 2122 2123
		mu 0 4 1145 1083 1085 1147
		f 4 -2119 2124 2125 -1992
		mu 0 4 1084 1146 1148 1086
		f 4 2126 2127 -2121 -2124
		mu 0 4 1147 1149 1141 1145
		f 4 2128 -2125 -2122 2129
		mu 0 4 1150 1148 1146 1142
		f 3 -1608 2130 -1959
		mu 0 3 892 870 1065
		f 3 -1609 -1962 2131
		mu 0 3 926 932 1067
		f 4 2132 -1651 1962 1963
		mu 0 4 1079 870 869 1072
		f 4 2133 1964 1965 -1617
		mu 0 4 926 1082 1074 925
		f 4 -2133 1978 -1971 -2131
		mu 0 4 870 1079 1075 1065
		f 4 -2134 -2132 -1973 1979
		mu 0 4 1082 926 1067 1076
		f 4 2134 2202 2194 1363
		mu 0 4 750 1151 1152 747
		f 4 2137 2219 2211 2139
		mu 0 4 1124 1153 1154 1155
		f 4 -2072 2140 -2136 -2068
		mu 0 4 1121 1125 1156 1123
		f 4 -2073 -2070 -2140 2141
		mu 0 4 1126 1122 1124 1155
		f 4 2203 2195 1389 -2195
		mu 0 4 1152 1157 742 747
		f 4 2220 2212 2145 -2212
		mu 0 4 1154 1158 1159 1155
		f 4 2146 -2144 -2141 -2082
		mu 0 4 1129 1160 1156 1125
		f 4 2147 -2084 -2142 -2146
		mu 0 4 1159 1130 1126 1155
		f 4 2148 -1967 -1588 2149
		mu 0 4 1161 1071 878 877
		f 4 2150 2151 -1592 -1969
		mu 0 4 1073 1162 928 927
		f 4 -1594 -2043 2152 -2150
		mu 0 4 877 882 1111 1161
		f 4 -1598 -2152 2153 -2045
		mu 0 4 929 928 1162 1112
		f 4 -2153 2154 2155 2156
		mu 0 4 1161 1111 1163 1164
		f 4 -2154 2157 2158 2159
		mu 0 4 1112 1162 1165 1166
		f 4 -2193 2201 -2135 1369
		mu 0 4 754 1167 1151 750
		f 4 -2210 2218 -2138 -2069
		mu 0 4 1120 1168 1153 1124
		f 4 1375 2164 -2156 -2161
		mu 0 4 759 718 1164 1163
		f 4 -1378 -2163 -2159 2165
		mu 0 4 761 757 1166 1165
		f 4 2166 2167 -2149 -2157
		mu 0 4 1164 1169 1071 1161
		f 4 2168 -2158 -2151 2169
		mu 0 4 1170 1165 1162 1073
		f 4 -2167 -2165 -1402 2170
		mu 0 4 1169 1164 718 717
		f 4 -2169 2171 -1338 -2166
		mu 0 4 1165 1170 720 761
		f 4 2174 2208 -2173 1339
		mu 0 4 726 1171 1172 723
		f 4 2175 2225 -2174 -2126
		mu 0 4 1148 1173 1174 1086
		f 4 2207 -2175 1345 2176
		mu 0 4 1175 1171 726 730
		f 4 2224 -2176 -2129 2177
		mu 0 4 1176 1173 1148 1150
		f 4 2178 2179 -2147 -2089
		mu 0 4 1133 1177 1160 1129
		f 4 2180 -2091 -2148 2181
		mu 0 4 1178 1134 1130 1159
		f 4 -2196 2204 2196 1357
		mu 0 4 742 1157 1179 738
		f 4 -2213 2221 2213 -2182
		mu 0 4 1159 1158 1180 1178
		f 4 2184 -2179 -2097 2185
		mu 0 4 1860 1177 1133 1862
		f 4 2186 2187 -2099 -2181
		mu 0 4 1178 1182 1138 1134
		f 4 -2186 -2106 -2128 2188
		mu 0 4 1181 1137 1141 1149
		f 4 -2188 2189 -2130 -2107
		mu 0 4 1861 1856 1150 1142
		f 4 -2197 2205 2197 1351
		mu 0 4 738 1179 1857 735
		f 4 -2214 2222 2214 -2187
		mu 0 4 1178 1180 1855 1182
		f 4 -2198 2206 -2177 1395
		mu 0 4 1858 1183 1175 730
		f 4 -2215 2223 -2178 -2190
		mu 0 4 1856 1184 1176 1150
		f 4 -2202 -2162 -2067 -2194
		mu 0 4 1151 1167 1119 1123
		f 4 -2203 2193 2135 2136
		mu 0 4 1152 1151 1123 1156
		f 4 2142 -2204 -2137 2143
		mu 0 4 1160 1157 1152 1156
		f 4 -2205 -2143 -2180 2182
		mu 0 4 1179 1157 1160 1177
		f 4 -2206 -2183 -2185 2190
		mu 0 4 1857 1179 1177 1860
		f 4 -2207 -2191 -2189 -2199
		mu 0 4 1175 1183 1181 1149
		f 4 -2200 -2208 2198 -2127
		mu 0 4 1147 1171 1175 1149
		f 4 -2209 2199 -2123 -2201
		mu 0 4 1172 1171 1147 1085
		f 4 -2219 -2164 -1372 -2211
		mu 0 4 1153 1168 756 751
		f 4 -2220 2210 -1366 2138
		mu 0 4 1154 1153 751 745
		f 4 2144 -2221 -2139 -1393
		mu 0 4 744 1158 1154 745
		f 4 -2222 -2145 -1360 2183
		mu 0 4 1180 1158 744 739
		f 4 -2223 -2184 -1354 2191
		mu 0 4 1855 1180 739 1859
		f 4 -2224 -2192 -1399 -2216
		mu 0 4 1176 1184 733 732
		f 4 -2217 -2225 2215 -1348
		mu 0 4 727 1173 1176 732
		f 4 -2226 2216 -1342 -2218
		mu 0 4 1174 1173 727 721
		f 4 -2171 -1381 2172 2226
		mu 0 4 1169 717 723 1172
		f 4 -2172 2227 2217 -1405
		mu 0 4 720 1170 1174 721
		f 4 -1989 -2168 -2227 2200
		mu 0 4 1085 1071 1169 1172
		f 4 -1991 2173 -2228 -2170
		mu 0 4 1073 1086 1174 1170
		f 4 2161 2228 -2155 -2059
		mu 0 4 1119 1167 1163 1111
		f 4 2209 -2061 -2160 2229
		mu 0 4 1168 1120 1112 1166
		f 4 2160 -2229 2192 1383
		mu 0 4 759 1163 1167 754
		f 4 2162 -1387 2163 -2230
		mu 0 4 1166 757 756 1168
		f 4 2230 -106 2231 2232
		mu 0 4 1185 59 58 1186
		f 4 2233 2234 2235 521
		mu 0 4 276 1187 1188 272
		f 4 -2255 -108 -2231 2236
		mu 0 4 629 48 59 1185
		f 4 -2256 2237 -2234 515
		mu 0 4 275 628 1187 276
		f 4 -1470 2238 -2233 2239
		mu 0 4 829 828 1185 1186
		f 4 1472 2240 -2235 2241
		mu 0 4 803 802 1188 1187
		f 4 -2239 -1499 2242 -2237
		mu 0 4 1185 828 827 629
		f 4 -2242 -2238 2243 1499
		mu 0 4 803 1187 628 797
		f 4 2244 2245 -1475 -2240
		mu 0 4 1186 1189 830 829
		f 4 2246 -2241 1476 2247
		mu 0 4 1190 1188 802 806
		f 4 -88 2248 1215 2249
		mu 0 4 50 49 637 645
		f 4 -519 2250 -1214 2251
		mu 0 4 278 258 644 636
		f 4 -90 -2250 1231 2252
		mu 0 4 51 50 645 653
		f 4 494 2253 -1230 -2251
		mu 0 4 258 257 652 644
		f 4 1199 -2249 -86 2254
		mu 0 4 629 637 49 48
		f 4 -1198 2255 -528 -2252
		mu 0 4 636 628 275 278
		f 4 2256 1181 -2243 -1465
		mu 0 4 826 623 629 827
		f 4 2257 1466 -2244 -1180
		mu 0 4 622 793 797 628
		f 4 1173 -2257 -1460 2258
		mu 0 4 619 623 826 825
		f 4 -1172 2259 1462 -2258
		mu 0 4 622 618 794 793
		f 4 1165 -2259 -1501 2260
		mu 0 4 615 619 825 824
		f 4 -1164 2261 -1494 -2260
		mu 0 4 618 613 822 794
		f 4 2262 -2261 -1457 2263
		mu 0 4 1191 615 824 823
		f 4 2264 2265 -1502 -2262
		mu 0 4 613 1192 819 822
		f 4 2266 -2264 -1489 2267
		mu 0 4 1193 1191 823 834
		f 4 2268 2269 1491 -2266
		mu 0 4 1192 1194 820 819
		f 4 -1495 2270 2271 -2268
		mu 0 4 834 833 1195 1193
		f 4 1495 -2270 2272 2273
		mu 0 4 815 820 1194 1196
		f 4 -1485 2274 2275 -2271
		mu 0 4 833 832 1197 1195
		f 4 1486 -2274 2276 2277
		mu 0 4 811 815 1196 1198
		f 4 2278 -2275 -1480 2279
		mu 0 4 1199 1197 832 831
		f 4 2280 2281 1482 -2278
		mu 0 4 1198 1200 812 811
		f 4 -1497 -2246 2282 -2280
		mu 0 4 831 830 1189 1199
		f 4 1497 -2282 2283 -2248
		mu 0 4 806 812 1200 1190
		f 4 2284 -2232 -104 2285
		mu 0 4 1201 1186 58 57
		f 4 2286 2287 512 -2236
		mu 0 4 1188 1202 269 272
		f 4 2288 -2286 -102 2289
		mu 0 4 1203 1201 57 56
		f 4 2290 2291 508 -2288
		mu 0 4 1202 1204 270 269
		f 4 2292 -2290 -100 2293
		mu 0 4 1205 1203 56 55
		f 4 2294 2295 523 -2292
		mu 0 4 1204 1206 266 270
		f 4 -98 2296 2297 -2294
		mu 0 4 55 54 1207 1205
		f 4 505 -2296 2298 2299
		mu 0 4 263 266 1206 1208
		f 4 2300 -2297 -96 2301
		mu 0 4 1209 1207 54 53
		f 4 2302 2303 501 -2300
		mu 0 4 1208 1210 264 263
		f 4 2304 -2302 -94 2305
		mu 0 4 1211 1209 53 52
		f 4 2306 2307 525 -2304
		mu 0 4 1210 1212 260 264
		f 4 2308 -2306 -92 -2253
		mu 0 4 653 1211 52 51
		f 4 2309 -2254 498 -2308
		mu 0 4 1212 652 257 260
		f 4 2310 1157 -2263 2311
		mu 0 4 1213 614 615 1191
		f 4 2312 2313 -2265 -1156
		mu 0 4 612 1214 1192 613
		f 4 2314 2315 -1241 -2311
		mu 0 4 1213 1215 656 614
		f 4 2316 -2313 -1240 2317
		mu 0 4 1216 1214 612 604
		f 4 3333 3319 1241 1143
		mu 0 4 606 1217 657 605
		f 4 3362 3348 -2318 -3348
		mu 0 4 603 1218 1216 604
		f 4 -3320 3334 -1274 -1250
		mu 0 4 657 1217 1782 664
		f 4 -3349 3363 3349 2321
		mu 0 4 1216 1218 1781 681
		f 4 -2315 2322 1283 -2321
		mu 0 4 1215 1213 688 687
		f 4 -2317 -2322 1284 2323
		mu 0 4 1214 1216 681 690
		f 4 -2323 -2312 -2267 2324
		mu 0 4 688 1213 1191 1193
		f 4 -2324 2325 -2269 -2314
		mu 0 4 1214 690 1194 1192
		f 4 -1287 -2325 -2272 2326
		mu 0 4 685 688 1193 1195
		f 4 -1289 2327 -2273 -2326
		mu 0 4 690 689 1196 1194
		f 4 -1290 -2327 -2276 2328
		mu 0 4 691 685 1195 1197
		f 4 -1293 2329 -2277 -2328
		mu 0 4 689 693 1198 1196
		f 4 2330 -1306 -2329 2331
		mu 0 4 1219 701 691 1197
		f 4 2332 2333 -2330 -1308
		mu 0 4 703 1220 1198 693
		f 4 2334 -1320 -2331 2335
		mu 0 4 1221 708 701 1219
		f 4 2336 2337 -2333 -1322
		mu 0 4 709 1222 1220 703
		f 4 -2332 -2279 2338 2339
		mu 0 4 1219 1197 1199 1223
		f 4 -2334 2340 2341 -2281
		mu 0 4 1198 1220 1224 1200
		f 4 2342 -2339 -2283 2343
		mu 0 4 1225 1223 1199 1189
		f 4 2344 2345 -2284 -2342
		mu 0 4 1224 1226 1190 1200
		f 4 -2344 -2245 2346 2347
		mu 0 4 1225 1189 1186 1227
		f 4 -2346 2348 2349 -2247
		mu 0 4 1190 1226 1228 1188
		f 4 2350 2351 -2347 -2285
		mu 0 4 1201 1229 1227 1186
		f 4 2352 -2287 -2350 2353
		mu 0 4 1230 1202 1188 1228
		f 4 2354 2355 -2351 -2289
		mu 0 4 1203 1231 1229 1201
		f 4 2356 -2291 -2353 2357
		mu 0 4 1232 1204 1202 1230
		f 4 2358 -2355 -2293 2359
		mu 0 4 1233 1231 1203 1205
		f 4 2360 2361 -2295 -2357
		mu 0 4 1232 1234 1206 1204
		f 4 -2340 2362 2363 -2336
		mu 0 4 1219 1223 1235 1221
		f 4 -2341 -2338 2365 -2365
		mu 0 4 1224 1220 1222 1236
		f 4 -2343 2366 2367 -2363
		mu 0 4 1223 1225 1237 1235
		f 4 -2345 2364 2369 -2369
		mu 0 4 1226 1224 1236 1238
		f 4 -2348 2370 2371 -2367
		mu 0 4 1225 1227 1239 1237
		f 4 -2349 2368 2373 -2373
		mu 0 4 1228 1226 1238 1240
		f 4 -2352 2374 2375 -2371
		mu 0 4 1227 1229 1241 1239
		f 4 -2354 2372 2377 -2377
		mu 0 4 1230 1228 1240 1242
		f 4 -2356 2378 2379 -2375
		mu 0 4 1229 1231 1243 1241
		f 4 -2358 2376 2381 -2381
		mu 0 4 1232 1230 1242 1244
		f 4 -2359 2382 2383 -2379
		mu 0 4 1231 1233 1245 1243
		f 4 -2361 2380 2385 -2385
		mu 0 4 1234 1232 1244 1246
		f 4 -2335 2386 2387 -1333
		mu 0 4 708 1221 1247 713
		f 4 -2337 -1334 2389 -2389
		mu 0 4 1222 709 714 1248
		f 4 -2364 2390 2391 -2387
		mu 0 4 1221 1235 1249 1247
		f 4 -2366 2388 2393 -2393
		mu 0 4 1236 1222 1248 1250
		f 4 -2368 2394 2395 -2391
		mu 0 4 1235 1237 1251 1249
		f 4 -2370 2392 2397 -2397
		mu 0 4 1238 1236 1250 1252
		f 4 -2372 2398 2399 -2395
		mu 0 4 1237 1239 1253 1251
		f 4 -2374 2396 2401 -2401
		mu 0 4 1240 1238 1252 1254
		f 4 -2376 2402 2403 -2399
		mu 0 4 1239 1241 1255 1253
		f 4 -2378 2400 2405 -2405
		mu 0 4 1242 1240 1254 1256
		f 4 -2380 2406 2407 -2403
		mu 0 4 1241 1243 1257 1255
		f 4 -2382 2404 2409 -2409
		mu 0 4 1244 1242 1256 1258
		f 4 -2384 2410 2411 -2407
		mu 0 4 1243 1245 1259 1257
		f 4 -2386 2408 2413 -2413
		mu 0 4 1246 1244 1258 1260
		f 4 -2388 2414 2415 -1330
		mu 0 4 713 1247 1261 712
		f 4 -2390 -1332 2417 -2417
		mu 0 4 1248 714 711 1262
		f 4 -2392 2418 2419 -2415
		mu 0 4 1247 1249 1263 1261
		f 4 -2394 2416 2421 -2421
		mu 0 4 1250 1248 1262 1264
		f 4 -2396 2422 2423 -2419
		mu 0 4 1249 1251 1265 1263
		f 4 -2398 2420 2425 -2425
		mu 0 4 1252 1250 1264 1266
		f 4 -2400 2426 2427 -2423
		mu 0 4 1251 1253 1267 1265
		f 4 -2402 2424 2429 -2429
		mu 0 4 1254 1252 1266 1268
		f 4 -2404 2430 2431 -2427
		mu 0 4 1253 1255 1269 1267
		f 4 -2406 2428 2433 -2433
		mu 0 4 1256 1254 1268 1270
		f 4 -2408 2434 2435 -2431
		mu 0 4 1255 1257 1271 1269
		f 4 -2410 2432 2437 -2437
		mu 0 4 1258 1256 1270 1272
		f 4 -2412 2438 2439 -2435
		mu 0 4 1257 1259 1273 1271
		f 4 -2414 2436 2441 -2441
		mu 0 4 1260 1258 1272 1274
		f 4 -2416 2442 2443 -1328
		mu 0 4 712 1261 1275 710
		f 4 -2418 -1326 -2444 -2445
		mu 0 4 1262 711 1700 1699
		f 4 -2420 2445 2446 -2443
		mu 0 4 1261 1263 1276 1275
		f 4 -2422 2444 -2447 -2448
		mu 0 4 1264 1262 1699 1698
		f 4 -2424 2448 2449 -2446
		mu 0 4 1263 1265 1277 1276
		f 4 -2426 2447 -2450 -2451
		mu 0 4 1266 1264 1698 1697
		f 4 -2428 2451 2452 -2449
		mu 0 4 1265 1267 1278 1277
		f 4 -2430 2450 -2453 -2454
		mu 0 4 1268 1266 1697 1696
		f 4 -2432 2454 2455 -2452
		mu 0 4 1267 1269 1279 1278
		f 4 -2434 2453 -2456 -2457
		mu 0 4 1270 1268 1696 1695
		f 4 -2436 2457 2458 -2455
		mu 0 4 1269 1271 1280 1279
		f 4 -2438 2456 -2459 -2460
		mu 0 4 1272 1270 1695 1694
		f 4 -2440 2460 2461 -2458
		mu 0 4 1271 1273 1281 1280
		f 4 -2442 2459 -2462 -2463
		mu 0 4 1274 1272 1694 1693
		f 4 2463 2464 2465 -1226
		mu 0 4 648 1282 1283 649
		f 4 2466 -1229 2467 2468
		mu 0 4 1284 651 650 1285
		f 4 -2466 2469 2470 -1237
		mu 0 4 649 1283 1286 655
		f 4 -2468 -1235 2471 2472
		mu 0 4 1285 650 654 1287
		f 4 2473 -1239 -2471 2474
		mu 0 4 1288 490 655 1286
		f 4 2475 2476 -2472 -1238
		mu 0 4 488 1289 1287 654
		f 4 2477 -948 -2474 2478
		mu 0 4 1290 491 490 1288
		f 4 2479 2480 -2476 -944
		mu 0 4 487 1291 1289 488
		f 4 2481 -958 -2478 2482
		mu 0 4 1292 495 491 1290
		f 4 2483 2484 -2480 -957
		mu 0 4 494 1293 1291 487
		f 4 -2482 2485 2486 -955
		mu 0 4 495 1292 1294 493
		f 4 -2484 -952 -2487 2487
		mu 0 4 1293 494 493 1294
		f 4 -2470 -2465 2489 -2489
		mu 0 4 1286 1283 1282 1295
		f 4 -2473 2490 2491 -2469
		mu 0 4 1285 1287 1296 1284
		f 4 -2475 2488 2493 -2493
		mu 0 4 1288 1286 1295 1297
		f 4 -2477 2494 2495 -2491
		mu 0 4 1287 1289 1298 1296
		f 4 -2479 2492 2497 -2497
		mu 0 4 1290 1288 1297 1299
		f 4 -2481 2498 2499 -2495
		mu 0 4 1289 1291 1300 1298
		f 4 -2483 2496 2501 -2501
		mu 0 4 1292 1290 1299 1301
		f 4 -2485 2502 2503 -2499
		mu 0 4 1291 1293 1302 1300
		f 4 -2486 2500 2505 -2505
		mu 0 4 1294 1292 1301 1303
		f 4 -2488 2504 2506 -2503
		mu 0 4 1293 1294 1303 1302
		f 4 -2464 -1231 2507 2508
		mu 0 4 1282 648 652 1304
		f 4 -2467 2509 2510 -1233
		mu 0 4 651 1284 1305 653
		f 4 2511 2512 -2490 -2509
		mu 0 4 1304 1306 1295 1282
		f 4 2513 -2510 -2492 2514
		mu 0 4 1307 1305 1284 1296
		f 4 2515 -2494 -2513 2516
		mu 0 4 1308 1297 1295 1306
		f 4 2517 2518 -2515 -2496
		mu 0 4 1298 1309 1307 1296
		f 4 2519 -2498 -2516 2520
		mu 0 4 1310 1299 1297 1308
		f 4 2521 2522 -2518 -2500
		mu 0 4 1300 1311 1309 1298
		f 4 2523 -2502 -2520 2524
		mu 0 4 1312 1301 1299 1310
		f 4 2525 2526 -2522 -2504
		mu 0 4 1302 1313 1311 1300
		f 4 2527 -2506 -2524 2528
		mu 0 4 1314 1303 1301 1312
		f 4 -2528 2529 -2526 -2507
		mu 0 4 1303 1314 1313 1302
		f 4 -2511 2530 2531 -2309
		mu 0 4 653 1305 1315 1211
		f 4 -2508 -2310 2532 2533
		mu 0 4 1304 652 1212 1316
		f 4 -2512 -2534 2535 -2535
		mu 0 4 1306 1304 1316 1317
		f 4 -2514 2536 2537 -2531
		mu 0 4 1305 1307 1318 1315
		f 4 -2517 2534 2539 -2539
		mu 0 4 1308 1306 1317 1319
		f 4 -2519 2540 2541 -2537
		mu 0 4 1307 1309 1320 1318
		f 4 -2521 2538 2543 -2543
		mu 0 4 1310 1308 1319 1321
		f 4 -2523 2544 2545 -2541
		mu 0 4 1309 1311 1322 1320
		f 4 -2525 2542 2547 -2547
		mu 0 4 1312 1310 1321 1323
		f 4 -2527 2548 2549 -2545
		mu 0 4 1311 1313 1324 1322
		f 4 -2529 2546 2551 -2551
		mu 0 4 1314 1312 1323 1325
		f 4 -2530 2550 2552 -2549
		mu 0 4 1313 1314 1325 1324
		f 4 -2532 2553 2554 -2305
		mu 0 4 1211 1315 1759 1209
		f 4 -2533 -2307 2556 -2556
		mu 0 4 1316 1212 1210 1327
		f 4 -2536 2555 2558 -2558
		mu 0 4 1317 1316 1327 1780
		f 4 -2538 2559 2560 -2554
		mu 0 4 1315 1318 1779 1759
		f 4 -2540 2557 2562 -2562
		mu 0 4 1319 1317 1780 1778
		f 4 -2542 2563 2564 -2560
		mu 0 4 1318 1320 1777 1779
		f 4 -2544 2561 2566 -2566
		mu 0 4 1321 1319 1778 1332
		f 4 -2546 2567 2568 -2564
		mu 0 4 1320 1322 1333 1777
		f 4 -2552 2569 2735 -2574
		mu 0 4 1325 1323 1334 1335
		f 4 -2553 2573 2736 -2572
		mu 0 4 1324 1325 1335 1336
		f 4 -2575 2576 2577 2578
		mu 0 4 1337 1338 1767 1771
		f 4 -2576 -2579 2580 2579
		mu 0 4 1341 1337 1771 1770
		f 4 2581 2582 2583 -2581
		mu 0 4 1772 1691 1344 1769
		f 4 -2582 -2578 2584 2585
		mu 0 4 1343 1340 1768 1345
		f 4 2588 -2587 2589 2590
		mu 0 4 1346 1347 1348 1692
		f 4 2591 2592 -2590 -2588
		mu 0 4 1350 1351 1349 1690
		f 4 2593 -2591 2594 -2461
		mu 0 4 1273 1346 1692 1281
		f 4 2595 2462 -2595 -2593
		mu 0 4 1351 1274 1693 1349
		f 4 2596 2440 -2596 2597
		mu 0 4 1352 1260 1274 1351
		f 4 2598 2599 -2594 -2439
		mu 0 4 1259 1353 1346 1273
		f 4 2600 -2598 -2592 2601
		mu 0 4 1354 1352 1351 1350
		f 4 2602 2603 -2589 -2600
		mu 0 4 1353 1355 1347 1346
		f 4 2604 -2599 2605 2606
		mu 0 4 1765 1353 1259 1357
		f 4 2607 2608 2609 -2597
		mu 0 4 1352 1766 1753 1260
		f 4 -2601 2610 2611 -2608
		mu 0 4 1352 1354 1360 1766
		f 4 -2603 -2605 2612 2613
		mu 0 4 1355 1353 1765 1361
		f 4 2616 2617 -2612 2618
		mu 0 4 1362 1363 1358 1764
		f 4 2619 2620 -2613 2621
		mu 0 4 1947 1365 1761 1949
		f 4 2622 -2609 -2618 2623
		mu 0 4 1366 1359 1950 1948
		f 4 2624 2625 -2622 -2607
		mu 0 4 1756 1367 1364 1356
		f 4 -2362 -2629 2630 -2299
		mu 0 4 1206 1234 1368 1208
		f 4 -2360 -2298 2631 -2630
		mu 0 4 1233 1205 1207 1369
		f 4 2669 2668 -2631 -2668
		mu 0 4 1370 1757 1208 1368
		f 4 2634 -2665 2666 2665
		mu 0 4 1372 1373 1374 1760
		f 4 -2559 -2637 2638 2639
		mu 0 4 1328 1758 1376 1377
		f 4 -2561 2640 2641 -2638
		mu 0 4 1326 1329 1378 1379
		f 4 2642 2643 -2563 -2640
		mu 0 4 1377 1380 1330 1328
		f 4 2644 -2641 -2565 2645
		mu 0 4 1381 1378 1329 1331
		f 4 2646 -2619 -2615 2647
		mu 0 4 1382 1362 1764 1763
		f 4 2648 2649 -2616 -2621
		mu 0 4 1365 1383 1762 1761
		f 4 -2585 2650 2651 -2648
		mu 0 4 1763 1339 1384 1382
		f 4 -2584 -2650 2652 2653
		mu 0 4 1342 1762 1383 1385
		f 4 2654 -2651 -2577 2655
		mu 0 4 1386 1384 1339 1774
		f 4 -2580 -2654 2656 2657
		mu 0 4 1773 1342 1385 1387
		f 4 -2656 -2571 2658 2659
		mu 0 4 1386 1774 1776 1388
		f 4 -2658 2660 2661 -2573
		mu 0 4 1773 1387 1389 1775
		f 4 2662 -2659 -2567 -2644
		mu 0 4 1380 1388 1776 1330
		f 4 2663 -2646 -2569 -2662
		mu 0 4 1389 1381 1331 1775
		f 4 -2667 -2628 -2632 2635
		mu 0 4 1375 1755 1369 1207
		f 4 2632 2633 -2670 -2627
		mu 0 4 1390 1391 1371 1754
		f 4 2670 -2636 -2301 -2555
		mu 0 4 1759 1375 1207 1209
		f 4 2671 -2557 -2303 -2669
		mu 0 4 1757 1327 1210 1208
		f 4 -2666 -2671 2637 2672
		mu 0 4 1372 1760 1326 1379
		f 4 -2634 2673 2636 -2672
		mu 0 4 1371 1391 1376 1758
		f 4 2674 2675 -2383 2629
		mu 0 4 1369 1392 1245 1233
		f 4 2676 2628 2384 2677
		mu 0 4 1393 1368 1234 1246
		f 4 -2606 -2411 -2676 2678
		mu 0 4 1357 1259 1245 1392
		f 4 -2610 2679 -2678 2412
		mu 0 4 1260 1753 1393 1246
		f 4 -2675 2627 2680 -2679
		mu 0 4 1392 1369 1755 1357
		f 4 -2677 -2680 2681 2667
		mu 0 4 1368 1393 1753 1370
		f 4 2682 -2625 -2681 2664
		mu 0 4 1373 1367 1756 1374
		f 4 2683 2626 -2682 -2623
		mu 0 4 1366 1390 1754 1359
		f 4 -2617 2684 2686 -2686
		mu 0 4 1363 1362 1394 1395
		f 4 -2620 2687 2689 -2689
		mu 0 4 1365 1947 1938 1397
		f 4 -2624 2685 2691 -2691
		mu 0 4 1366 1948 1937 1398
		f 4 -2626 2692 2693 -2688
		mu 0 4 1364 1367 1399 1396
		f 4 -2633 2694 2696 -2696
		mu 0 4 1391 1390 1400 1401
		f 4 -2635 2697 2699 -2699
		mu 0 4 1373 1372 1402 1403
		f 4 -2639 2700 2702 -2702
		mu 0 4 1377 1376 1404 1405
		f 4 -2642 2703 2705 -2705
		mu 0 4 1379 1378 1406 1407
		f 4 -2643 2701 2707 -2707
		mu 0 4 1380 1377 1405 1408
		f 4 -2645 2708 2709 -2704
		mu 0 4 1378 1381 1409 1406
		f 4 -2647 2710 2711 -2685
		mu 0 4 1362 1382 1410 1394
		f 4 -2649 2688 2713 -2713
		mu 0 4 1383 1365 1397 1411
		f 4 -2652 2714 2715 -2711
		mu 0 4 1382 1384 1412 1410
		f 4 -2653 2712 2717 -2717
		mu 0 4 1385 1383 1411 1413
		f 4 -2655 2718 2719 -2715
		mu 0 4 1384 1386 1414 1412
		f 4 -2657 2716 2721 -2721
		mu 0 4 1387 1385 1413 1415
		f 4 -2660 2722 2723 -2719
		mu 0 4 1386 1388 1416 1414
		f 4 -2661 2720 2725 -2725
		mu 0 4 1389 1387 1415 1417
		f 4 -2663 2706 2726 -2723
		mu 0 4 1388 1380 1408 1416
		f 4 -2664 2724 2727 -2709
		mu 0 4 1381 1389 1417 1409
		f 4 -2673 2704 2728 -2698
		mu 0 4 1372 1379 1407 1402
		f 4 -2674 2695 2729 -2701
		mu 0 4 1376 1391 1401 1404
		f 4 -2683 2698 2730 -2693
		mu 0 4 1367 1373 1403 1399
		f 4 -2684 2690 2731 -2695
		mu 0 4 1390 1366 1398 1400
		f 4 -2736 2732 2574 -2734
		mu 0 4 1335 1334 1338 1337
		f 4 -2737 2733 2575 -2735
		mu 0 4 1336 1335 1337 1341
		f 4 2565 2737 -2570 -2548
		mu 0 4 1321 1332 1334 1323
		f 4 -2568 -2550 2571 2738
		mu 0 4 1333 1322 1324 1336
		f 3 -2738 2570 -2733
		mu 0 3 1334 1332 1338
		f 3 -2739 2734 2572
		mu 0 3 1333 1336 1341
		f 4 -2583 2739 2740 2741
		mu 0 4 1344 1691 1689 1419
		f 4 -2586 2742 2743 -2740
		mu 0 4 1343 1345 1420 1418
		f 4 -2741 2744 2586 2745
		mu 0 4 1419 1689 1348 1347
		f 4 -2744 2746 2587 -2745
		mu 0 4 1418 1420 1350 1690
		f 4 -2746 -2604 -2614 2747
		mu 0 4 1419 1347 1355 1361
		f 4 -2747 2748 -2611 -2602
		mu 0 4 1350 1420 1360 1354
		f 3 -2742 -2748 2615
		mu 0 3 1344 1419 1361
		f 3 -2743 2614 -2749
		mu 0 3 1420 1345 1360
		f 4 2749 2759 2758 2752
		mu 0 4 1959 1960 1423 1424
		f 4 2753 2762 2761 2756
		mu 0 4 1952 1955 1427 1428
		f 4 -2760 2757 2750 2751
		mu 0 4 1423 1960 1956 1430
		f 4 -2763 2760 2754 2755
		mu 0 4 1427 1955 1953 1432
		f 4 2763 2764 -2759 2765
		mu 0 4 1433 1434 1424 1423
		f 4 2766 2767 -2756 2768
		mu 0 4 1435 1436 1427 1432
		f 4 2769 -2766 -2752 2770
		mu 0 4 1437 1433 1423 1430
		f 4 2771 2772 -2762 -2768
		mu 0 4 1436 1438 1428 1427
		f 4 2773 -2753 2774 2775
		mu 0 4 1439 1959 1424 1440
		f 4 2776 2777 2778 -2755
		mu 0 4 1953 1954 1442 1432
		f 4 2779 -2776 2780 2781
		mu 0 4 1946 1439 1440 1444
		f 4 2782 2783 2784 -2778
		mu 0 4 1954 1945 1446 1442
		f 4 2785 2786 -2775 -2765
		mu 0 4 1434 1447 1440 1424
		f 4 2787 -2769 -2779 2788
		mu 0 4 1448 1435 1432 1442
		f 4 2789 2790 2791 -2751
		mu 0 4 1956 1957 1450 1430
		f 4 2792 -2757 2793 2794
		mu 0 4 1451 1952 1428 1452
		f 4 -2792 2795 2796 -2771
		mu 0 4 1430 1450 1453 1437
		f 4 -2794 -2773 2797 2798
		mu 0 4 1452 1428 1438 1454
		f 4 2799 2800 2801 -2796
		mu 0 4 1450 1455 1456 1453
		f 4 2802 -2799 2803 2804
		mu 0 4 1457 1452 1454 1458
		f 4 2805 2806 -2781 -2787
		mu 0 4 1447 1459 1444 1440
		f 4 2807 -2789 -2785 2808
		mu 0 4 1460 1448 1442 1446
		f 4 2809 2810 -2800 -2791
		mu 0 4 1957 1944 1455 1450
		f 4 2811 -2795 -2803 2812
		mu 0 4 1943 1451 1452 1457
		f 4 2813 2814 -2782 -2807
		mu 0 4 1459 1926 1946 1444
		f 4 2815 -2809 -2784 2816
		mu 0 4 1923 1460 1446 1945
		f 4 2817 -2801 -2811 2818
		mu 0 4 1942 1456 1455 1944
		f 4 2819 2820 -2813 -2805
		mu 0 4 1458 1941 1943 1457
		f 4 2821 2822 2823 -2818
		mu 0 4 1942 1940 1468 1456
		f 4 2824 -2820 2825 2826
		mu 0 4 1939 1941 1458 1470
		f 4 2827 2828 2829 -2823
		mu 0 4 1940 1936 1472 1468
		f 4 2830 -2827 2831 2832
		mu 0 4 1933 1939 1470 1474
		f 4 -2830 2833 -2731 2834
		mu 0 4 1468 1472 1399 1403
		f 4 -2832 2835 -2732 2836
		mu 0 4 1474 1470 1400 1398
		f 4 -2700 2837 -2824 -2835
		mu 0 4 1403 1402 1456 1468
		f 4 -2697 -2836 -2826 2838
		mu 0 4 1401 1400 1470 1458
		f 4 -2694 -2834 2839 2840
		mu 0 4 1396 1399 1472 1935
		f 4 -2692 2841 2842 -2837
		mu 0 4 1398 1937 1934 1474
		f 4 -2840 -2829 2843 2844
		mu 0 4 1935 1472 1936 1931
		f 4 -2843 2845 2846 -2833
		mu 0 4 1474 1934 1930 1933
		f 4 -2690 -2841 2847 2848
		mu 0 4 1397 1938 1475 1479
		f 4 -2687 2849 2850 -2842
		mu 0 4 1395 1394 1480 1476
		f 4 2851 2852 -2848 -2845
		mu 0 4 1932 1928 1479 1475
		f 4 2853 -2846 -2851 2854
		mu 0 4 1927 1929 1476 1480
		f 4 2855 2856 -2714 -2849
		mu 0 4 1479 1483 1411 1397
		f 4 2857 -2850 -2712 2858
		mu 0 4 1484 1480 1394 1410
		f 4 -2856 -2853 2859 2860
		mu 0 4 1483 1479 1928 1925
		f 4 -2858 2861 2862 -2855
		mu 0 4 1480 1484 1924 1927
		f 4 2863 -2878 -2718 -2857
		mu 0 4 1483 1459 1413 1411
		f 4 2864 -2859 -2716 -2879
		mu 0 4 1460 1484 1410 1412
		f 4 2865 -2814 -2864 -2861
		mu 0 4 1925 1926 1459 1483
		f 4 2866 -2862 -2865 -2816
		mu 0 4 1923 1924 1484 1460
		f 4 -2802 -2838 -2729 2867
		mu 0 4 1453 1456 1402 1407
		f 4 -2804 2868 -2730 -2839
		mu 0 4 1458 1454 1404 1401
		f 4 -2797 -2868 -2706 2869
		mu 0 4 1437 1453 1407 1406
		f 4 -2798 2870 -2703 -2869
		mu 0 4 1454 1438 1405 1404
		f 4 -2770 -2870 -2710 2871
		mu 0 4 1433 1437 1406 1409
		f 4 -2772 2872 -2708 -2871
		mu 0 4 1438 1436 1408 1405
		f 4 -2728 2873 -2764 -2872
		mu 0 4 1409 1417 1434 1433
		f 4 -2727 -2873 -2767 2874
		mu 0 4 1416 1408 1436 1435
		f 4 -2786 -2874 -2726 2875
		mu 0 4 1447 1434 1417 1415
		f 4 -2788 2876 -2724 -2875
		mu 0 4 1435 1448 1414 1416
		f 4 -2722 2877 -2806 -2876
		mu 0 4 1415 1413 1459 1447
		f 4 -2720 -2877 -2808 2878
		mu 0 4 1412 1414 1448 1460
		f 4 2879 -2844 2880 2881
		mu 0 4 1487 1477 1471 1488
		f 4 2882 2883 2884 -2847
		mu 0 4 1478 1489 1490 1473
		f 4 2885 -2852 -2880 2886
		mu 0 4 1491 1481 1477 1487
		f 4 2887 2888 -2883 -2854
		mu 0 4 1482 1492 1489 1478
		f 4 2889 2890 2891 -2758
		mu 0 4 1422 1493 1494 1429
		f 4 2892 -2754 2893 2894
		mu 0 4 1495 1426 1425 1496
		f 4 2895 2896 -2890 -2750
		mu 0 4 1421 1497 1493 1422
		f 4 2897 -2761 -2893 2898
		mu 0 4 1498 1431 1426 1495
		f 4 2899 2900 2901 -2897
		mu 0 4 1497 1499 1500 1493
		f 4 2902 -2899 2903 2904
		mu 0 4 1501 1498 1495 1502
		f 4 2905 -2891 -2902 2906
		mu 0 4 1503 1494 1493 1500
		f 4 2907 2908 -2904 -2895
		mu 0 4 1496 1504 1502 1495
		f 4 2909 2920 2918 -2882
		mu 0 4 1488 1505 1506 1487
		f 4 2911 2925 2923 -2909
		mu 0 4 1504 1507 1508 1502
		f 4 2921 2919 -2887 -2919
		mu 0 4 1506 1509 1491 1487
		f 4 2926 2924 -2905 -2924
		mu 0 4 1508 1510 1501 1502
		f 4 -2810 -2790 -2892 2915
		mu 0 4 1461 1449 1429 1494
		f 4 -2812 2916 -2894 -2793
		mu 0 4 1951 1462 1496 1425
		f 4 -2921 2917 -2907 2910
		mu 0 4 1506 1505 1503 1500
		f 4 2913 -2922 -2911 -2901
		mu 0 4 1499 1509 1506 1500
		f 4 -2926 2922 -2884 2912
		mu 0 4 1508 1507 1490 1489
		f 4 2914 -2927 -2913 -2889
		mu 0 4 1492 1510 1508 1489
		f 4 -2906 2927 -2819 -2916
		mu 0 4 1494 1503 1465 1461
		f 4 -2908 -2917 -2821 2928
		mu 0 4 1504 1496 1462 1466
		f 4 2929 -2822 -2928 -2918
		mu 0 4 1505 1467 1465 1503
		f 4 2930 -2912 -2929 -2825
		mu 0 4 1469 1507 1504 1466
		f 4 -2881 -2828 -2930 -2910
		mu 0 4 1488 1471 1467 1505
		f 4 -2885 -2923 -2931 -2831
		mu 0 4 1473 1490 1507 1469
		f 4 -2780 2931 -2896 -2774
		mu 0 4 1958 1443 1497 1421
		f 4 -2783 -2777 -2898 2932
		mu 0 4 1445 1441 1431 1498
		f 4 -2900 -2932 -2815 2933
		mu 0 4 1499 1497 1443 1463
		f 4 -2903 2934 -2817 -2933
		mu 0 4 1498 1501 1464 1445
		f 4 -2934 -2866 2935 -2914
		mu 0 4 1499 1463 1485 1509
		f 4 -2935 -2925 2936 -2867
		mu 0 4 1464 1501 1510 1486
		f 4 -2886 -2920 -2936 -2860
		mu 0 4 1481 1491 1509 1485
		f 4 -2888 -2863 -2937 -2915
		mu 0 4 1492 1482 1486 1510
		f 4 2937 2938 -1553 2939
		mu 0 4 1511 1512 838 837
		f 4 2940 2941 -1555 -2939
		mu 0 4 1512 1513 840 838
		f 4 2942 -2940 -1557 2943
		mu 0 4 1514 1511 837 842
		f 4 2944 2945 -1559 -2942
		mu 0 4 1513 1515 844 840
		f 4 2946 -2944 -1561 2947
		mu 0 4 1516 1514 842 846
		f 4 2948 2949 -1563 -2946
		mu 0 4 1515 1517 848 844
		f 4 2950 -2948 -1565 2951
		mu 0 4 1518 1516 846 850
		f 4 2952 2953 -1567 -2950
		mu 0 4 1517 1519 852 848
		f 4 -1569 2954 2955 -2952
		mu 0 4 850 854 1520 1518
		f 4 -1571 -2954 2956 2957
		mu 0 4 856 852 1519 1521
		f 4 2958 2959 -2955 -1573
		mu 0 4 858 1522 1520 854
		f 4 2960 -1575 -2958 2961
		mu 0 4 1523 860 856 1521
		f 4 2962 -2959 -1577 2963
		mu 0 4 1524 1522 858 862
		f 4 2964 2965 -1579 -2961
		mu 0 4 1523 1525 864 860
		f 4 -1581 2966 2967 -2964
		mu 0 4 862 866 1526 1524
		f 4 -1582 -2966 2968 -2967
		mu 0 4 866 864 1525 1526
		f 4 2969 2970 3021 3019
		mu 0 4 1527 1528 1529 1530
		f 4 2972 -3020 3022 3020
		mu 0 4 1531 1527 1530 1532
		f 4 2974 2975 -2970 2976
		mu 0 4 1533 1534 1528 1527
		f 4 2977 -2977 -2973 2978
		mu 0 4 1535 1533 1527 1531
		f 4 2979 2980 -2975 2981
		mu 0 4 1536 1537 1534 1533
		f 4 2982 -2982 -2978 2983
		mu 0 4 1538 1536 1533 1535
		f 4 2984 2990 2988 -2938
		mu 0 4 1511 1539 1540 1512
		f 4 2991 2989 -2941 -2989
		mu 0 4 1540 1541 1513 1512
		f 4 -2991 2987 -2980 2985
		mu 0 4 1540 1539 1537 1536
		f 4 2986 -2992 -2986 -2983
		mu 0 4 1538 1541 1540 1536
		f 4 2992 -2985 -2943 2993
		mu 0 4 1542 1539 1511 1514
		f 4 2994 2995 -2945 -2990
		mu 0 4 1541 1543 1515 1513
		f 4 2996 2997 -2994 -2947
		mu 0 4 1516 1544 1542 1514
		f 4 2998 -2949 -2996 2999
		mu 0 4 1545 1517 1515 1543
		f 4 -2951 3000 3001 -2997
		mu 0 4 1516 1518 1546 1544
		f 4 -2953 -2999 3002 3003
		mu 0 4 1519 1517 1545 1547
		f 4 3004 -3001 -2956 3005
		mu 0 4 1548 1546 1518 1520
		f 4 3006 3007 -2957 -3004
		mu 0 4 1547 1549 1521 1519
		f 4 -2993 -2998 3008 -2988
		mu 0 4 1539 1542 1544 1537
		f 4 -2995 -2987 3009 -3000
		mu 0 4 1543 1541 1538 1545
		f 4 -3009 -3002 3010 -2981
		mu 0 4 1537 1544 1546 1534
		f 4 -3010 -2984 3011 -3003
		mu 0 4 1545 1538 1535 1547
		f 4 3012 -2976 -3011 -3005
		mu 0 4 1548 1528 1534 1546
		f 4 3013 -3007 -3012 -2979
		mu 0 4 1531 1549 1547 1535
		f 4 3014 3015 -3006 -2960
		mu 0 4 1522 1550 1548 1520
		f 4 3016 -2962 -3008 3017
		mu 0 4 1551 1523 1521 1549;
	setAttr ".fc[1500:1697]"
		f 4 -3022 3018 -2968 2971
		mu 0 4 1530 1529 1524 1526
		f 4 -3023 -2972 -2969 2973
		mu 0 4 1532 1530 1526 1525
		f 4 3023 -3015 -2963 -3019
		mu 0 4 1529 1550 1522 1524
		f 4 3024 -2974 -2965 -3017
		mu 0 4 1551 1532 1525 1523
		f 4 -3013 -3016 -3024 -2971
		mu 0 4 1528 1548 1550 1529
		f 4 -3014 -3021 -3025 -3018
		mu 0 4 1549 1531 1532 1551
		f 4 -3042 3025 865 866
		mu 0 4 440 439 836 835
		f 4 -3043 -867 869 870
		mu 0 4 443 440 835 839
		f 4 874 875 -3044 -871
		mu 0 4 839 843 446 443
		f 4 880 881 -3045 -876
		mu 0 4 843 847 450 446
		f 4 -3046 -882 887 888
		mu 0 4 455 450 847 851
		f 4 -3047 -889 893 894
		mu 0 4 459 455 851 855
		f 4 -3048 -895 899 900
		mu 0 4 463 459 855 859
		f 4 904 905 -3049 -901
		mu 0 4 859 863 466 463
		f 4 910 -3035 -3050 -906
		mu 0 4 863 865 468 466
		f 4 907 -3036 -3051 3034
		mu 0 4 865 861 464 468
		f 4 901 -3037 -3052 3035
		mu 0 4 861 857 461 464
		f 4 -3053 3036 897 -3038
		mu 0 4 457 461 857 853
		f 4 -3054 3037 891 -3039
		mu 0 4 453 457 853 849
		f 4 -3055 3038 885 -3040
		mu 0 4 448 453 849 845
		f 4 877 -3041 -3056 3039
		mu 0 4 845 841 444 448
		f 4 871 -3026 -3057 3040
		mu 0 4 841 836 439 444
		f 4 3265 3273 -3220 -3222
		mu 0 4 1552 1553 1554 1555
		f 4 3277 3274 3294 -3261
		mu 0 4 1556 1557 1558 1559
		f 4 -913 3057 3059 -3059
		mu 0 4 471 470 1560 1561
		f 4 -919 3060 3061 -3058
		mu 0 4 470 474 1562 1560
		f 4 -971 3062 3063 -3061
		mu 0 4 474 502 1563 1562
		f 4 -1084 3064 3065 -3063
		mu 0 4 502 570 1564 1563
		f 4 -1078 3066 3067 -3065
		mu 0 4 570 566 1565 1564
		f 4 -1072 3068 3069 -3067
		mu 0 4 566 562 1566 1565
		f 4 -1066 3070 3071 -3069
		mu 0 4 562 558 1567 1566
		f 4 -1060 3072 3073 -3071
		mu 0 4 558 555 1568 1567
		f 4 -1055 3074 3075 -3073
		mu 0 4 555 554 1569 1568
		f 4 3329 -1048 3076 3077
		mu 0 4 1570 548 543 1571
		f 4 -1043 3078 3079 -3077
		mu 0 4 543 538 1572 1571
		f 4 -1036 3080 3081 -3079
		mu 0 4 538 534 1573 1572
		f 4 -1030 3082 3083 -3081
		mu 0 4 534 531 1574 1573
		f 4 -1025 3084 3085 -3083
		mu 0 4 531 524 1575 1574
		f 4 -1012 3086 3087 -3085
		mu 0 4 524 517 1576 1575
		f 4 -1000 3088 3089 -3087
		mu 0 4 517 504 1577 1576
		f 4 -975 3058 3090 -3089
		mu 0 4 504 471 1561 1577
		f 4 -916 3091 3093 -3093
		mu 0 4 473 472 1578 1579
		f 4 -978 3094 3095 -3092
		mu 0 4 472 506 1580 1578
		f 4 -998 3096 3097 -3095
		mu 0 4 506 516 1581 1580
		f 4 1009 3098 -3100 -3097
		mu 0 4 516 523 1582 1581
		f 4 1022 3100 -3102 -3099
		mu 0 4 523 530 1583 1582
		f 4 -1027 3102 3103 -3101
		mu 0 4 530 532 1584 1583
		f 4 -1033 3104 3105 -3103
		mu 0 4 532 536 1585 1584
		f 4 -1041 3106 3107 -3105
		mu 0 4 536 541 1586 1585
		f 4 3358 -1045 3108 3109
		mu 0 4 1587 545 544 1588
		f 4 -1053 3110 3111 -3109
		mu 0 4 544 551 1589 1588
		f 4 -1057 3112 3113 -3111
		mu 0 4 551 556 1590 1589
		f 4 -1063 3114 3115 -3113
		mu 0 4 556 560 1591 1590
		f 4 -1069 3116 3117 -3115
		mu 0 4 560 564 1592 1591
		f 4 -1075 3118 3119 -3117
		mu 0 4 564 568 1593 1592
		f 4 -1082 3120 3121 -3119
		mu 0 4 568 503 1594 1593
		f 4 -973 3122 3123 -3121
		mu 0 4 503 476 1595 1594
		f 4 -922 3092 3124 -3123
		mu 0 4 476 473 1579 1595
		f 4 -3060 3125 3127 -3127
		mu 0 4 1561 1560 1596 1597
		f 4 -3062 3128 3129 -3126
		mu 0 4 1560 1562 1598 1596
		f 4 -3064 3130 3131 -3129
		mu 0 4 1562 1563 1599 1598
		f 4 -3066 3132 3133 -3131
		mu 0 4 1563 1564 1600 1599
		f 4 -3068 3134 3135 -3133
		mu 0 4 1564 1565 1601 1600
		f 4 -3070 3136 3137 -3135
		mu 0 4 1565 1566 1602 1601
		f 4 -3072 3138 3139 -3137
		mu 0 4 1566 1567 1603 1602
		f 4 -3074 3140 3141 -3139
		mu 0 4 1567 1568 1604 1603
		f 4 -3076 3142 3143 -3141
		mu 0 4 1568 1569 1605 1604
		f 4 3328 -3078 3144 3145
		mu 0 4 1606 1570 1571 1607
		f 4 -3080 3146 3147 -3145
		mu 0 4 1571 1572 1608 1607
		f 4 -3082 3148 3149 -3147
		mu 0 4 1572 1573 1609 1608
		f 4 -3084 3150 3151 -3149
		mu 0 4 1573 1574 1610 1609
		f 4 -3086 3152 3153 -3151
		mu 0 4 1574 1575 1611 1610
		f 4 -3088 3154 3155 -3153
		mu 0 4 1575 1576 1612 1611
		f 4 -3090 3156 3157 -3155
		mu 0 4 1576 1577 1613 1612
		f 4 -3091 3126 3158 -3157
		mu 0 4 1577 1561 1597 1613
		f 4 -3094 3159 3161 -3161
		mu 0 4 1579 1578 1614 1615
		f 4 -3096 3162 3163 -3160
		mu 0 4 1578 1580 1616 1614
		f 4 -3098 3164 3165 -3163
		mu 0 4 1580 1581 1617 1616
		f 4 3099 3166 -3168 -3165
		mu 0 4 1581 1582 1618 1617
		f 4 3101 3168 -3170 -3167
		mu 0 4 1582 1583 1619 1618
		f 4 -3104 3170 3171 -3169
		mu 0 4 1583 1584 1620 1619
		f 4 -3106 3172 3173 -3171
		mu 0 4 1584 1585 1621 1620
		f 4 -3108 3174 3175 -3173
		mu 0 4 1585 1586 1622 1621
		f 4 3357 -3110 3176 3177
		mu 0 4 1623 1587 1588 1624
		f 4 -3112 3178 3179 -3177
		mu 0 4 1588 1589 1625 1624
		f 4 -3114 3180 3181 -3179
		mu 0 4 1589 1590 1626 1625
		f 4 -3116 3182 3183 -3181
		mu 0 4 1590 1591 1627 1626
		f 4 -3118 3184 3185 -3183
		mu 0 4 1591 1592 1628 1627
		f 4 -3120 3186 3187 -3185
		mu 0 4 1592 1593 1629 1628
		f 4 -3122 3188 3189 -3187
		mu 0 4 1593 1594 1630 1629
		f 4 -3124 3190 3191 -3189
		mu 0 4 1594 1595 1631 1630
		f 4 -3125 3160 3192 -3191
		mu 0 4 1595 1579 1615 1631
		f 4 -3128 3193 3195 -3195
		mu 0 4 1597 1596 1632 1633
		f 4 -3130 3196 3197 -3194
		mu 0 4 1596 1598 1634 1632
		f 4 -3132 3198 3199 -3197
		mu 0 4 1598 1599 1635 1634
		f 4 -3134 3200 3201 -3199
		mu 0 4 1599 1600 1636 1635
		f 4 -3136 3202 3203 -3201
		mu 0 4 1600 1601 1637 1636
		f 4 -3138 3204 3205 -3203
		mu 0 4 1601 1602 1638 1637
		f 4 -3140 3206 3207 -3205
		mu 0 4 1602 1603 1639 1638
		f 4 -3142 3208 3209 -3207
		mu 0 4 1603 1604 1640 1639
		f 4 -3144 3210 3211 -3209
		mu 0 4 1604 1605 1641 1640
		f 4 3327 -3146 3212 3213
		mu 0 4 1642 1606 1607 1643
		f 4 -3148 3214 3215 -3213
		mu 0 4 1607 1608 1644 1643
		f 4 -3150 3216 3217 -3215
		mu 0 4 1608 1609 1554 1644
		f 4 -3152 3218 3219 -3217
		mu 0 4 1609 1610 1555 1554
		f 4 -3154 3220 3221 -3219
		mu 0 4 1610 1611 1552 1555
		f 4 -3156 3222 3223 -3221
		mu 0 4 1611 1612 1645 1552
		f 4 -3158 3224 3225 -3223
		mu 0 4 1612 1613 1646 1645
		f 4 -3159 3194 3226 -3225
		mu 0 4 1613 1597 1633 1646
		f 4 -3162 3227 3229 -3229
		mu 0 4 1615 1614 1647 1556
		f 4 -3164 3230 3231 -3228
		mu 0 4 1614 1616 1648 1647
		f 4 -3166 3232 3233 -3231
		mu 0 4 1616 1617 1649 1648
		f 4 3167 3234 -3236 -3233
		mu 0 4 1617 1618 1650 1649
		f 4 3169 3236 -3238 -3235
		mu 0 4 1618 1619 1651 1650
		f 4 -3172 3238 3239 -3237
		mu 0 4 1619 1620 1652 1651
		f 4 -3174 3240 3241 -3239
		mu 0 4 1620 1621 1653 1652
		f 4 -3176 3242 3243 -3241
		mu 0 4 1621 1622 1654 1653
		f 4 3356 -3178 3244 3245
		mu 0 4 1655 1623 1624 1656
		f 4 -3180 3246 3247 -3245
		mu 0 4 1624 1625 1657 1656
		f 4 -3182 3248 3249 -3247
		mu 0 4 1625 1626 1658 1657
		f 4 -3184 3250 3251 -3249
		mu 0 4 1626 1627 1659 1658
		f 4 -3186 3252 3253 -3251
		mu 0 4 1627 1628 1660 1659
		f 4 -3188 3254 3255 -3253
		mu 0 4 1628 1629 1661 1660
		f 4 -3190 3256 3257 -3255
		mu 0 4 1629 1630 1662 1661
		f 4 -3192 3258 3259 -3257
		mu 0 4 1630 1631 1559 1662
		f 4 -3193 3228 3260 -3259
		mu 0 4 1631 1615 1556 1559
		f 4 3263 3271 -3262 -3226
		mu 0 4 1646 1663 1664 1645
		f 4 3292 -3276 3279 3276
		mu 0 4 1665 1666 1667 1668
		f 4 -3196 3270 -3264 -3227
		mu 0 4 1633 1632 1663 1646
		f 4 3293 -3275 3278 3275
		mu 0 4 1666 1558 1557 1667
		f 4 3261 3272 -3266 -3224
		mu 0 4 1645 1664 1553 1552
		f 4 3291 -3277 3280 -3242
		mu 0 4 1653 1665 1668 1652
		f 4 3367 -3204 -3206 -3310
		mu 0 4 1669 1636 1637 1638
		f 4 3300 3297 3324 -3297
		mu 0 4 1670 1671 1669 1672
		f 4 3299 3296 3325 -3296
		mu 0 4 1673 1670 1672 1674
		f 4 3298 3295 3326 -3214
		mu 0 4 1643 1673 1674 1642
		f 4 -3230 -3232 3264 -3278
		mu 0 4 1556 1647 1648 1557
		f 4 -3279 -3265 -3234 3262
		mu 0 4 1667 1557 1648 1649
		f 4 -3280 -3263 3235 3266
		mu 0 4 1668 1667 1649 1650
		f 4 -3281 -3267 3237 -3240
		mu 0 4 1652 1668 1650 1651
		f 4 -3274 3269 -3285 -3218
		mu 0 4 1554 1553 1675 1644
		f 4 -3273 3268 -3286 -3270
		mu 0 4 1553 1664 1676 1675
		f 4 -3272 3267 -3287 -3269
		mu 0 4 1664 1663 1677 1676
		f 4 -3271 -3198 -3288 -3268
		mu 0 4 1663 1632 1634 1677
		f 4 3305 -3289 -3292 -3244
		mu 0 4 1654 1678 1665 1653
		f 4 3306 -3290 -3293 3288
		mu 0 4 1678 1679 1666 1665
		f 4 3307 -3291 -3294 3289
		mu 0 4 1679 1680 1558 1666
		f 4 -3295 3290 3308 -3260
		mu 0 4 1559 1558 1680 1662
		f 4 3284 3281 -3299 -3216
		mu 0 4 1644 1675 1673 1643
		f 4 3285 3282 -3300 -3282
		mu 0 4 1675 1676 1670 1673
		f 4 3286 3283 -3301 -3283
		mu 0 4 1676 1677 1671 1670
		f 4 3287 -3200 -3302 -3284
		mu 0 4 1677 1634 1635 1671
		f 4 -3341 3355 -3246 -3248
		mu 0 4 1657 1681 1655 1656
		f 4 -3340 3354 3340 -3250
		mu 0 4 1658 1682 1681 1657
		f 4 -3339 3353 3339 -3252
		mu 0 4 1659 1683 1682 1658
		f 4 -3309 3304 3368 -3258
		mu 0 4 1662 1680 1683 1661
		f 4 -3325 3309 -3208 -3311
		mu 0 4 1672 1669 1638 1639
		f 4 -3326 3310 -3210 -3312
		mu 0 4 1674 1672 1639 1640
		f 4 -3327 3311 -3212 -3313
		mu 0 4 1642 1674 1640 1641
		f 4 -3314 -3328 3312 -3211
		mu 0 4 1605 1606 1642 1641
		f 4 -3315 -3329 3313 -3143
		mu 0 4 1569 1570 1606 1605
		f 4 -3316 -3330 3314 -3075
		mu 0 4 554 548 1570 1569
		f 4 -3331 3315 1048 1049
		mu 0 4 549 548 554 553
		f 4 -3332 -1050 1140 -3318
		mu 0 4 602 549 553 608
		f 4 -3333 3317 1144 -3319
		mu 0 4 606 602 608 656
		f 4 2318 -3334 3318 -2316
		mu 0 4 1215 1217 606 656
		f 4 -3335 -2319 2320 -3321
		mu 0 4 1782 1217 1215 687
		f 4 -3336 3320 1276 -3322
		mu 0 4 677 676 687 686
		f 4 -3337 3321 1280 -3323
		mu 0 4 682 677 686 692
		f 4 1301 1302 -3338 3322
		mu 0 4 692 702 698 682
		f 4 -3354 -3305 -3308 3303
		mu 0 4 1682 1683 1680 1679
		f 4 -3355 -3304 -3307 3302
		mu 0 4 1681 1682 1679 1678
		f 4 -3356 -3303 -3306 -3342
		mu 0 4 1655 1681 1678 1654
		f 4 -3343 -3357 3341 -3243
		mu 0 4 1622 1623 1655 1654
		f 4 -3344 -3358 3342 -3175
		mu 0 4 1586 1587 1623 1622
		f 4 -3345 -3359 3343 -3107
		mu 0 4 541 545 1587 1586
		f 4 -3360 3344 -1040 1045
		mu 0 4 546 545 541 540
		f 4 -1129 1137 -3361 -1046
		mu 0 4 540 598 600 546
		f 4 -1135 1141 -3362 -1138
		mu 0 4 598 662 603 600
		f 4 2319 -3363 -1142 1242
		mu 0 4 661 1218 603 662
		f 4 -3364 -2320 -1255 1274
		mu 0 4 1781 1218 661 1783
		f 4 1261 1277 -3365 -1275
		mu 0 4 667 671 679 678
		f 4 1269 1281 -3366 -1278
		mu 0 4 671 675 683 679
		f 4 -3367 -1282 -1301 1304
		mu 0 4 700 683 675 697
		f 4 3301 -3202 -3368 -3298
		mu 0 4 1671 1635 1636 1669
		f 4 -3369 3338 -3254 -3256
		mu 0 4 1661 1683 1659 1660
		f 4 3369 3370 -1126 3371
		mu 0 4 1684 1685 574 597
		f 4 3372 3373 -1124 3374
		mu 0 4 1686 1687 596 573
		f 4 -3371 3375 3376 -1089
		mu 0 4 574 1685 1688 572
		f 4 -3375 -1086 -3377 3377
		mu 0 4 1686 573 572 1688
		f 3 3378 -1128 -3374
		mu 0 3 1687 598 596
		f 3 3379 -3372 -1130
		mu 0 3 599 1684 597
		f 4 3380 -3378 3381 1089
		mu 0 4 575 1686 1688 576
		f 4 3382 1090 -3382 -3376
		mu 0 4 1685 579 576 1688
		f 4 3383 -3373 -3381 1131
		mu 0 4 663 1687 1686 575
		f 4 3384 1132 -3383 -3370
		mu 0 4 1684 659 579 1685
		f 4 -3384 1133 1134 -3379
		mu 0 4 1687 663 662 598
		f 4 -3385 -3380 1135 1136
		mu 0 4 659 1684 599 605;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "buddy_teeth" -p "geometry_GRP";
	rename -uid "07092E96-4640-D296-77AC-F8B277F75F81";
	setAttr -l on ".tx";
	setAttr -l on ".ty";
	setAttr -l on ".tz";
	setAttr -l on ".rx";
	setAttr -l on ".ry";
	setAttr -l on ".rz";
	setAttr -l on ".sx";
	setAttr -l on ".sy";
	setAttr -l on ".sz";
	setAttr ".it" no;
createNode mesh -n "buddy_teethShape" -p "buddy_teeth";
	rename -uid "E6CA5605-474D-A7F2-5B73-CC8CD974B11A";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".pv" -type "double2" 0.045808181488746436 0.95278013621127933 ;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
	setAttr ".dr" 3;
	setAttr ".dsm" 2;
	setAttr ".vcs" 2;
createNode mesh -n "buddy_teethShapeOrig" -p "buddy_teeth";
	rename -uid "8CC14337-47A5-15A5-91A3-32A429CF62D8";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 154 ".uvst[0].uvsp[0:153]" -type "float2" 0.030471146 0.95692182
		 0.022310451 0.95651901 0.022310451 0.95305395 0.030183375 0.95316571 0.022310331
		 0.94941127 0.030126452 0.9490732 0.037396282 0.94298851 0.037293524 0.93107963 0.035757989
		 0.97038496 0.034552962 0.95614374 0.031189829 0.96994406 0.015426241 0.95628786 0.013999194
		 0.96814346 0.0097941346 0.96753132 0.011941511 0.95520133 0.0048962459 0.96677649
		 0.0088344999 0.9443084 0.029721349 0.91444707 0.022310495 0.91460651 0.022310376
		 0.98607403 0.030464262 0.98642957 0.022310317 0.98174757 0.030398697 0.98258936 0.033033937
		 0.94239533 0.033139259 0.93143755 0.029549152 0.94209427 0.029530853 0.93168974 0.028985828
		 0.92161167 0.022310495 0.92204869 0.022310346 0.91862601 0.0291318 0.91844469 0.015406743
		 0.93254644 0.016000599 0.94261962 0.012816571 0.94323677 0.011932701 0.93273336 0.022310436
		 0.94590306 0.029679239 0.9453131 0.022310317 0.94275171 0.022310317 0.96917129 0.022310317
		 0.93210727 0.03324002 0.95287031 0.035150558 0.9483875 0.015744112 0.95291615 0.013109285
		 0.95255321 0.011483628 0.9488731 0.01577254 0.94920987 0.033376127 0.98633897 0.03364405
		 0.91634917 0.034247309 0.98349559 0.035890967 0.92004752 0.0094705522 0.98044127
		 0.0097810179 0.98355263 0.010487691 0.91816431 0.0085719526 0.92200476 0.012893617
		 0.98462069 0.014129937 0.91601008 0.013544217 0.98065293 0.031531245 0.91873783 0.032116979
		 0.92108536 0.012773231 0.92020661 0.012364581 0.92253608 0.01514703 0.91962421 0.015511692
		 0.92262608 0.032474607 0.94515234 0.01354428 0.94573212 0.016040999 0.94567776 0.061737895
		 0.95596313 0.062062383 0.95221204 0.069913149 0.95259452 0.069913268 0.95607334 0.062152624
		 0.94812077 0.069913268 0.94894612 0.052353978 0.95308542 0.057671785 0.95491934 0.056170225
		 0.96918124 0.055408478 0.92985266 0.060829401 0.96902102 0.07678926 0.95628196 0.080287874
		 0.95542741 0.082231641 0.96778524 0.078084469 0.96814275 0.083444238 0.94475186 0.087077737
		 0.96734846 0.060512424 0.99124366 0.061206698 0.98607397 0.069913149 0.98571861 0.069913149
		 0.91431665 0.061375141 0.98199135 0.069913149 0.98136115 0.059442282 0.93047529 0.059335351
		 0.94128793 0.06293726 0.93093389 0.062782645 0.94121039 0.063647985 0.92120445 0.063567042
		 0.91829026 0.069913149 0.91830903 0.069913149 0.92170775 0.076984763 0.93248928 0.080533266
		 0.93288124 0.079473674 0.94341218 0.076277792 0.94258249 0.062624693 0.9443996 0.069913208
		 0.94544238 0.069913149 0.94229996 0.069913149 0.96871662 0.069913149 0.93170035 0.057176113
		 0.94711369 0.059021354 0.9517197 0.076490581 0.95290089 0.076478601 0.94919628 0.08078891
		 0.94914865 0.079134405 0.95271498 0.059574604 0.91597325 0.058093667 0.98573977 0.057179689
		 0.91928494 0.057314157 0.98269236 0.082247794 0.98045301 0.086999595 0.98092693 0.082578003
		 0.91792232 0.081849039 0.98340446 0.078542292 0.91503119 0.078897357 0.98424703 0.078334332
		 0.9804675 0.060717225 0.92054117 0.061362386 0.91839302 0.080344856 0.92239982 0.079998732
		 0.91990381 0.077038109 0.922351 0.077461362 0.91913772 0.059859037 0.94406039 0.078730762
		 0.94586861 0.076222301 0.94564581 0.069913149 0.99086231 0.063053727 0.91483849 0.084628463
		 0.93331003 0.08432281 0.92209578 0.050663829 0.96945423 0.054997444 0.94159102 0.084829509
		 0.98586828 0.080320716 0.98881859 0.05538702 0.98890895 0.05235815 0.9840818 0.08472389
		 0.95385033 0.022310495 0.9911778 0.031012625 0.99107766 0.0079115219 0.93290061 0.0045386255
		 0.98061997 0.04117164 0.9710201 0.039923996 0.95466852 0.0064841658 0.98590434 0.011351287
		 0.98964012 0.035713285 0.98958623 0.038954884 0.9851892 0.0075546689 0.95332599;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 132 ".vt[0:131]"  -1.1404413 30.39941216 18.90256691 -1.56815827 30.33769798 18.79614449
		 -1.64304698 30.53408432 19.25580597 -1.58589721 30.94445801 19.44617844 -1.15298843 30.82846069 19.36222649
		 -0.87511182 30.75400543 18.94664764 -3.65857601 31.074144363 18.20403099 -3.71106195 31.51389503 18.15995026
		 -3.6711235 31.5031929 18.66369247 -3.33675885 31.41360092 18.96048737 -3.3939085 31.0032272339 18.7701149
		 -3.31901979 30.80683899 18.31045341 -0.22084522 33.83139038 18.90256691 -0.1683594 33.39163971 18.94664764
		 -0.44623613 33.46609879 19.36222649 -0.87914491 33.5820961 19.44617844 -0.72345102 33.96606445 19.25580597
		 -0.56040084 34.098697662 18.79614449 -2.73898005 34.50612259 18.20403099 -2.31126237 34.56784058 18.31045341
		 -2.47431254 34.43520737 18.7701149 -2.63000631 34.051235199 18.96048737 -2.96437097 34.14083099 18.66369247
		 -3.0043094158 34.15153122 18.15995026 -0.20829797 33.40234375 18.44290543 -0.48551285 33.90230942 18.336483
		 -0.54266262 33.49193573 18.14611053 -2.7264328 34.077075958 17.74437141 -2.29352427 33.96107864 17.66041946
		 -2.23637438 34.37145233 17.85079193 -0.91505039 30.76470566 18.44290543 -1.24941516 30.85429955 18.14611053
		 -1.40510893 30.47032928 18.336483 -3.15597057 30.9394722 17.85079193 -3.00027656555 31.32344055 17.66041946
		 -3.4331851 31.43943787 17.74437141 -1.28849649 30.51732254 19.25819969 -3.66561556 31.15427017 18.59878349
		 -0.40801883 33.80331039 19.25819969 -2.78513765 34.44025803 18.59878349 -0.21380448 33.75127029 18.50781441
		 -2.59092331 34.38821793 17.84839821 -1.094282389 30.46528244 18.50781441 -3.47140121 31.10223007 17.84839821
		 -0.7996124 32.14727783 19.36222649 -0.52173567 32.072822571 18.94664764 -0.56167424 32.083522797 18.44290543
		 -0.89603877 32.17311859 18.14611053 -2.64690042 32.64225769 17.66041946 -3.079809189 32.758255 17.74437141
		 -3.35768557 32.83271408 18.15995026 -3.31774712 32.82201004 18.66369247 -2.98338246 32.73241806 18.96048737
		 -1.23252106 32.26327515 19.44617844 -2.56929326 30.69356346 19.012960434 -2.51214361 31.10393715 19.2033329
		 -2.10795188 32.49784851 19.2033329 -1.75457561 33.81666565 19.2033329 -1.59888172 34.200634 19.012960434
		 -1.43583155 34.33327103 18.55329895 -1.36094368 34.13687897 18.093637466 -1.41809344 33.72650528 17.903265
		 -1.77146959 32.40768814 17.903265 -2.17566133 31.013776779 17.903265 -2.33135533 30.62980843 18.093637466
		 -2.49440455 30.49717522 18.55329895 1.1404413 30.39941216 18.90256691 1.56815827 30.33769798 18.79614449
		 1.64304698 30.53408432 19.25580597 1.58589721 30.94445801 19.44617844 1.15298843 30.82846069 19.36222649
		 0.87511182 30.75400543 18.94664764 3.65857601 31.074144363 18.20403099 3.71106195 31.51389503 18.15995026
		 3.6711235 31.5031929 18.66369247 3.33675885 31.41360092 18.96048737 3.3939085 31.0032272339 18.7701149
		 3.31901979 30.80683899 18.31045341 0.22084522 33.83139038 18.90256691 0.1683594 33.39163971 18.94664764
		 0.44623613 33.46609879 19.36222649 0.87914491 33.5820961 19.44617844 0.72345102 33.96606445 19.25580597
		 0.56040084 34.098697662 18.79614449 2.73898005 34.50612259 18.20403099 2.31126237 34.56784058 18.31045341
		 2.47431254 34.43520737 18.7701149 2.63000631 34.051235199 18.96048737 2.96437097 34.14083099 18.66369247
		 3.0043094158 34.15153122 18.15995026 0.20829797 33.40234375 18.44290543 0.48551285 33.90230942 18.336483
		 0.54266262 33.49193573 18.14611053 2.7264328 34.077075958 17.74437141 2.29352427 33.96107864 17.66041946
		 2.23637438 34.37145233 17.85079193 0.91505039 30.76470566 18.44290543 1.24941516 30.85429955 18.14611053
		 1.40510893 30.47032928 18.336483 3.15597057 30.9394722 17.85079193 3.00027656555 31.32344055 17.66041946
		 3.4331851 31.43943787 17.74437141 1.28849649 30.51732254 19.25819969 3.66561556 31.15427017 18.59878349
		 0.40801883 33.80331039 19.25819969 2.78513765 34.44025803 18.59878349 0.21380448 33.75127029 18.50781441
		 2.59092331 34.38821793 17.84839821 1.094282389 30.46528244 18.50781441 3.47140121 31.10223007 17.84839821
		 0.7996124 32.14727783 19.36222649 0.52173567 32.072822571 18.94664764 0.56167424 32.083522797 18.44290543
		 0.89603877 32.17311859 18.14611053 2.64690042 32.64225769 17.66041946 3.079809189 32.758255 17.74437141
		 3.35768557 32.83271408 18.15995026 3.31774712 32.82201004 18.66369247 2.98338246 32.73241806 18.96048737
		 1.23252106 32.26327515 19.44617844 2.56929326 30.69356346 19.012960434 2.51214361 31.10393715 19.2033329
		 2.10795188 32.49784851 19.2033329 1.75457561 33.81666565 19.2033329 1.59888172 34.200634 19.012960434
		 1.43583155 34.33327103 18.55329895 1.36094368 34.13687897 18.093637466 1.41809344 33.72650528 17.903265
		 1.77146959 32.40768814 17.903265 2.17566133 31.013776779 17.903265 2.33135533 30.62980843 18.093637466
		 2.49440455 30.49717522 18.55329895;
	setAttr -s 256 ".ed";
	setAttr ".ed[0:165]"  1 0 1 0 5 1 3 2 1 2 54 1 10 9 1 9 55 1 2 1 1 1 65 1
		 11 10 1 5 4 1 4 44 1 14 13 1 13 45 1 4 3 1 3 53 1 15 14 1 7 6 1 6 11 1 9 8 1 8 51 1
		 22 21 1 21 52 1 8 7 1 7 50 1 23 22 1 13 12 1 12 17 1 17 16 1 16 58 1 20 19 1 19 59 1
		 16 15 1 15 57 1 21 20 1 19 18 1 18 23 1 13 24 1 24 46 1 30 5 1 24 26 1 26 47 1 31 30 1
		 26 25 1 25 60 1 29 28 1 28 61 1 25 17 1 19 29 1 28 27 1 27 49 1 35 34 1 34 48 1 27 23 1
		 7 35 1 1 32 1 32 64 1 33 11 1 32 31 1 31 63 1 34 33 1 0 36 1 36 4 1 2 36 1 6 37 1
		 37 10 1 8 37 1 12 38 1 38 16 1 14 38 1 18 39 1 39 22 1 20 39 1 24 40 1 40 25 1 12 40 1
		 27 41 1 41 18 1 29 41 1 0 42 1 42 32 1 30 42 1 6 43 1 43 35 1 33 43 1 44 14 1 45 5 1
		 46 30 1 47 31 1 48 28 1 49 35 1 50 23 1 51 22 1 52 9 1 53 15 1 44 45 1 45 46 1 46 47 1
		 47 62 1 48 49 1 49 50 1 50 51 1 51 52 1 52 56 1 53 44 1 54 10 1 55 3 1 56 53 1 57 21 1
		 58 20 1 59 17 1 60 29 1 61 26 1 62 48 1 63 34 1 64 33 1 65 11 1 54 55 1 55 56 1 56 57 1
		 57 58 1 58 59 1 59 60 1 60 61 1 61 62 1 62 63 1 63 64 1 64 65 1 65 54 1 67 66 1 66 71 1
		 69 68 1 68 120 1 76 75 1 75 121 1 68 67 1 67 131 1 77 76 1 71 70 1 70 110 1 80 79 1
		 79 111 1 70 69 1 69 119 1 81 80 1 73 72 1 72 77 1 75 74 1 74 117 1 88 87 1 87 118 1
		 74 73 1 73 116 1 89 88 1 79 78 1 78 83 1 83 82 1 82 124 1 86 85 1 85 125 1 82 81 1
		 81 123 1 87 86 1 85 84 1 84 89 1 79 90 1 90 112 1;
	setAttr ".ed[166:255]" 96 71 1 90 92 1 92 113 1 97 96 1 92 91 1 91 126 1 95 94 1
		 94 127 1 91 83 1 85 95 1 94 93 1 93 115 1 101 100 1 100 114 1 93 89 1 73 101 1 67 98 1
		 98 130 1 99 77 1 98 97 1 97 129 1 100 99 1 66 102 1 102 70 1 68 102 1 72 103 1 103 76 1
		 74 103 1 78 104 1 104 82 1 80 104 1 84 105 1 105 88 1 86 105 1 90 106 1 106 91 1
		 78 106 1 93 107 1 107 84 1 95 107 1 66 108 1 108 98 1 96 108 1 72 109 1 109 101 1
		 99 109 1 110 80 1 111 71 1 112 96 1 113 97 1 114 94 1 115 101 1 116 89 1 117 88 1
		 118 75 1 119 81 1 110 111 1 111 112 1 112 113 1 113 128 1 114 115 1 115 116 1 116 117 1
		 117 118 1 118 122 1 119 110 1 120 76 1 121 69 1 122 119 1 123 87 1 124 86 1 125 83 1
		 126 95 1 127 92 1 128 114 1 129 100 1 130 99 1 131 77 1 120 121 1 121 122 1 122 123 1
		 123 124 1 124 125 1 125 126 1 126 127 1 127 128 1 128 129 1 129 130 1 130 131 1 131 120 1;
	setAttr -s 128 -ch 512 ".fc[0:127]" -type "polyFaces" 
		f 4 -106 -117 -4 -3
		mu 0 4 0 1 2 3
		f 4 3 -128 -8 -7
		mu 0 4 3 2 4 5
		f 4 -86 -95 -11 -10
		mu 0 4 148 147 8 9
		f 4 10 -104 -15 -14
		mu 0 4 9 8 10 0
		f 4 -93 -102 -20 -19
		mu 0 4 11 12 13 14
		f 4 19 -101 -24 -23
		mu 0 4 14 13 15 153
		f 4 -110 -121 -29 -28
		mu 0 4 144 143 19 20
		f 4 28 -120 -33 -32
		mu 0 4 20 19 21 22
		f 4 85 -39 -87 -96
		mu 0 4 7 6 23 24
		f 4 86 -42 -88 -97
		mu 0 4 24 23 25 26
		f 4 -112 -123 -44 -43
		mu 0 4 27 28 29 30
		f 4 43 -122 109 -47
		mu 0 4 30 29 18 17
		f 4 -52 -51 -90 -99
		mu 0 4 31 32 33 34
		f 4 89 -54 23 -100
		mu 0 4 34 33 16 145
		f 4 7 -127 -56 -55
		mu 0 4 5 4 35 36
		f 4 55 -126 -59 -58
		mu 0 4 36 35 37 25
		f 4 14 -107 -118 105
		mu 0 4 0 10 38 1
		f 4 87 58 -125 -98
		mu 0 4 26 25 37 39
		f 4 -62 -61 1 9
		mu 0 4 9 40 41 148
		f 4 60 -63 6 0
		mu 0 4 41 40 3 5
		f 4 62 61 13 2
		mu 0 4 3 40 9 0
		f 4 -65 -64 17 8
		mu 0 4 42 43 44 45
		f 4 63 -66 22 16
		mu 0 4 44 43 14 153
		f 4 65 64 4 18
		mu 0 4 14 43 42 11
		f 4 -68 -67 26 27
		mu 0 4 20 46 151 144
		f 4 66 -69 11 25
		mu 0 4 151 46 48 152
		f 4 68 67 31 15
		mu 0 4 48 46 20 22
		f 4 -71 -70 35 24
		mu 0 4 50 51 149 146
		f 4 69 -72 29 34
		mu 0 4 149 51 54 150
		f 4 71 70 20 33
		mu 0 4 54 51 50 56
		f 4 -74 -73 39 42
		mu 0 4 30 57 58 27
		f 4 72 -75 -26 36
		mu 0 4 58 57 47 49
		f 4 74 73 46 -27
		mu 0 4 47 57 30 17
		f 4 -77 -76 52 -36
		mu 0 4 52 59 60 53
		f 4 75 -78 44 48
		mu 0 4 60 59 61 62
		f 4 77 76 -35 47
		mu 0 4 61 59 52 55
		f 4 -80 -79 -1 54
		mu 0 4 36 63 41 5
		f 4 78 -81 38 -2
		mu 0 4 41 63 23 6
		f 4 80 79 57 41
		mu 0 4 23 63 36 25
		f 4 -83 -82 -17 53
		mu 0 4 33 64 44 16
		f 4 81 -84 56 -18
		mu 0 4 44 64 65 45
		f 4 83 82 50 59
		mu 0 4 65 64 33 32
		f 4 -13 -12 -85 94
		mu 0 4 147 152 48 8
		f 4 12 95 -38 -37
		mu 0 4 49 7 24 58
		f 4 37 96 -41 -40
		mu 0 4 58 24 26 27
		f 4 40 97 -124 111
		mu 0 4 27 26 39 28
		f 4 -89 98 -50 -49
		mu 0 4 62 31 34 60
		f 4 49 99 90 -53
		mu 0 4 60 34 145 53
		f 4 91 -25 -91 100
		mu 0 4 13 50 146 15
		f 4 -22 -21 -92 101
		mu 0 4 12 56 50 13
		f 4 93 32 -119 106
		mu 0 4 10 22 21 38
		f 4 84 -16 -94 103
		mu 0 4 8 48 22 10
		f 4 -6 -5 -105 116
		mu 0 4 1 11 42 2
		f 4 -103 92 5 117
		mu 0 4 38 12 11 1
		f 4 107 21 102 118
		mu 0 4 21 56 12 38
		f 4 108 -34 -108 119
		mu 0 4 19 54 56 21
		f 4 -31 -30 -109 120
		mu 0 4 143 150 54 19
		f 4 110 -48 30 121
		mu 0 4 29 61 55 18
		f 4 -46 -45 -111 122
		mu 0 4 28 62 61 29
		f 4 112 88 45 123
		mu 0 4 39 31 62 28
		f 4 113 51 -113 124
		mu 0 4 37 32 31 39
		f 4 114 -60 -114 125
		mu 0 4 35 65 32 37
		f 4 115 -57 -115 126
		mu 0 4 4 45 65 35
		f 4 104 -9 -116 127
		mu 0 4 2 42 45 4
		f 4 130 131 244 233
		mu 0 4 66 67 68 69
		f 4 134 135 255 -132
		mu 0 4 67 70 71 68
		f 4 137 138 222 213
		mu 0 4 72 73 74 136
		f 4 141 142 231 -139
		mu 0 4 73 66 76 74
		f 4 146 147 229 220
		mu 0 4 77 78 79 80
		f 4 150 151 228 -148
		mu 0 4 78 142 82 79
		f 4 155 156 248 237
		mu 0 4 83 84 85 132
		f 4 159 160 247 -157
		mu 0 4 84 87 88 85
		f 4 223 214 166 -214
		mu 0 4 75 89 90 137
		f 4 224 215 169 -215
		mu 0 4 89 91 92 90
		f 4 170 171 250 239
		mu 0 4 93 94 95 96
		f 4 174 -238 249 -172
		mu 0 4 94 133 86 95
		f 4 226 217 178 179
		mu 0 4 97 98 99 100
		f 4 227 -152 181 -218
		mu 0 4 98 134 81 99
		f 4 182 183 254 -136
		mu 0 4 70 101 102 71
		f 4 185 186 253 -184
		mu 0 4 101 92 103 102
		f 4 -234 245 234 -143
		mu 0 4 66 69 104 76
		f 4 225 252 -187 -216
		mu 0 4 91 105 103 92
		f 4 -138 -130 188 189
		mu 0 4 73 72 106 107
		f 4 -129 -135 190 -189
		mu 0 4 106 70 67 107
		f 4 -131 -142 -190 -191
		mu 0 4 67 66 73 107
		f 4 -137 -146 191 192
		mu 0 4 108 109 110 111
		f 4 -145 -151 193 -192
		mu 0 4 110 142 78 111
		f 4 -147 -133 -193 -194
		mu 0 4 78 77 108 111
		f 4 -156 -155 194 195
		mu 0 4 84 83 140 113
		f 4 -154 -140 196 -195
		mu 0 4 140 141 115 113
		f 4 -144 -160 -196 -197
		mu 0 4 115 87 84 113
		f 4 -153 -164 197 198
		mu 0 4 116 117 138 119
		f 4 -163 -158 199 -198
		mu 0 4 138 139 121 119
		f 4 -162 -149 -199 -200
		mu 0 4 121 122 116 119
		f 4 -171 -168 200 201
		mu 0 4 94 93 123 124
		f 4 -165 153 202 -201
		mu 0 4 123 114 112 124
		f 4 154 -175 -202 -203
		mu 0 4 112 133 94 124
		f 4 163 -181 203 204
		mu 0 4 118 135 125 126
		f 4 -177 -173 205 -204
		mu 0 4 125 127 128 126
		f 4 -176 162 -205 -206
		mu 0 4 128 120 118 126
		f 4 -183 128 206 207
		mu 0 4 101 70 106 129
		f 4 129 -167 208 -207
		mu 0 4 106 137 90 129
		f 4 -170 -186 -208 -209
		mu 0 4 90 92 101 129
		f 4 -182 144 209 210
		mu 0 4 99 81 110 130
		f 4 145 -185 211 -210
		mu 0 4 110 109 131 130
		f 4 -188 -179 -211 -212
		mu 0 4 131 100 99 130
		f 4 -223 212 139 140
		mu 0 4 136 74 115 141
		f 4 164 165 -224 -141
		mu 0 4 114 123 89 75
		f 4 167 168 -225 -166
		mu 0 4 123 93 91 89
		f 4 -240 251 -226 -169
		mu 0 4 93 96 105 91
		f 4 176 177 -227 216
		mu 0 4 127 125 98 97
		f 4 180 -219 -228 -178
		mu 0 4 125 135 134 98
		f 4 -229 218 152 -220
		mu 0 4 79 82 117 116
		f 4 -230 219 148 149
		mu 0 4 80 79 116 122
		f 4 -235 246 -161 -222
		mu 0 4 76 104 88 87
		f 4 -232 221 143 -213
		mu 0 4 74 76 87 115
		f 4 -245 232 132 133
		mu 0 4 69 68 108 77
		f 4 -246 -134 -221 230
		mu 0 4 104 69 77 80
		f 4 -247 -231 -150 -236
		mu 0 4 88 104 80 122
		f 4 -248 235 161 -237
		mu 0 4 85 88 122 121
		f 4 -249 236 157 158
		mu 0 4 132 85 121 139
		f 4 -250 -159 175 -239
		mu 0 4 95 86 120 128
		f 4 -251 238 172 173
		mu 0 4 96 95 128 127
		f 4 -252 -174 -217 -241
		mu 0 4 105 96 127 97
		f 4 -253 240 -180 -242
		mu 0 4 103 105 97 100
		f 4 -254 241 187 -243
		mu 0 4 102 103 100 131
		f 4 -255 242 184 -244
		mu 0 4 71 102 131 109
		f 4 -256 243 136 -233
		mu 0 4 68 71 109 108;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 10 
		4 0 
		5 0 
		41 0 
		44 0 
		45 0 
		70 0 
		71 0 
		106 0 
		109 0 
		110 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "buddy_tongue" -p "geometry_GRP";
	rename -uid "E976ACEC-4D80-5387-A741-A6BD078B5556";
	setAttr ".t" -type "double3" 0 27.697321864198447 15.397355930458216 ;
	setAttr -l on ".tx";
	setAttr -l on ".ty";
	setAttr -l on ".tz";
	setAttr -l on ".rx";
	setAttr -l on ".ry";
	setAttr -l on ".rz";
	setAttr ".s" -type "double3" 5.994676720366078 2.4938140804848508 7.003606517275113 ;
	setAttr -l on ".sx";
	setAttr -l on ".sy";
	setAttr -l on ".sz";
	setAttr ".spt" -type "double3" 0 0 1.6984449441868642e-16 ;
	setAttr ".it" no;
createNode mesh -n "buddy_tongueShape" -p "buddy_tongue";
	rename -uid "C069508D-437B-66BC-355F-1C8AEE306190";
	setAttr -k off ".v";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".pv" -type "double2" 0.12907106985854289 0.92258474720310113 ;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
	setAttr ".dr" 3;
	setAttr ".dsm" 2;
	setAttr ".vcs" 2;
createNode mesh -n "buddy_tongueShapeOrig" -p "buddy_tongue";
	rename -uid "A2BB3060-4D33-90CF-8CE7-2991EC44113C";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr -s 6 ".gtag";
	setAttr ".gtag[0].gtagnm" -type "string" "back";
	setAttr ".gtag[0].gtagcmp" -type "componentList" 1 "f[2]";
	setAttr ".gtag[1].gtagnm" -type "string" "bottom";
	setAttr ".gtag[1].gtagcmp" -type "componentList" 1 "f[3]";
	setAttr ".gtag[2].gtagnm" -type "string" "front";
	setAttr ".gtag[2].gtagcmp" -type "componentList" 1 "f[0]";
	setAttr ".gtag[3].gtagnm" -type "string" "left";
	setAttr ".gtag[3].gtagcmp" -type "componentList" 1 "f[5]";
	setAttr ".gtag[4].gtagnm" -type "string" "right";
	setAttr ".gtag[4].gtagcmp" -type "componentList" 1 "f[4]";
	setAttr ".gtag[5].gtagnm" -type "string" "top";
	setAttr ".gtag[5].gtagcmp" -type "componentList" 1 "f[1]";
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 31 ".uvst[0].uvsp[0:30]" -type "float2" 0.11133344 0.91479063
		 0.12907106 0.9165979 0.12907106 0.92312843 0.10641219 0.92314118 0.14680862 0.91479045
		 0.15172994 0.92314142 0.14678204 0.93150467 0.12907106 0.92965436 0.11136003 0.93150479
		 0.12907106 0.95330924 0.10617574 0.952775 0.15196633 0.95277506 0.14864993 0.9737401
		 0.12907106 0.98030949 0.10949214 0.9737401 0.12907106 0.85387146 0.10277046 0.86502451
		 0.15500557 0.98018306 0.14890611 0.87146038 0.12907106 0.8648454 0.10923596 0.87146038
		 0.12907106 0.89245027 0.10368891 0.8929888 0.15445316 0.8929888 0.16513783 0.95269442
		 0.093004063 0.95269442 0.092756346 0.89303893 0.10313655 0.98018306 0.15537167 0.86502451
		 0.16538578 0.89303893 0.12907106 0.99129802;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 26 ".vt[0:25]"  -0.37316319 -0.32704163 0.41329074 0.37316319 -0.32704163 0.41329074
		 -0.37316319 0.32704163 0.41329074 0.37316319 0.32704163 0.41329074 -0.37316319 0.32704163 -0.26710296
		 0.37316319 0.32704163 -0.26710296 -0.37316319 -0.32704163 -0.26710296 0.37316319 -0.32704163 -0.26710296
		 -0.50377011 0.60763836 0.073093891 0.42788535 0 -0.31699085 0 -0.44150543 -0.38617182
		 -0.42788535 0 -0.31699085 0.50377011 -0.44150543 0.073093891 -0.50377011 -0.44150543 0.073093891
		 0 -0.44150543 0.5323596 0.42788535 0 0.46317863 0 0.44150543 0.5323596 -0.42788535 0 0.46317863
		 0.50377011 0.60763836 0.073093891 0 0.44150543 -0.38617182 0 0 0.59320688 0 0.75480747 0.073093891
		 0 0 -0.4470191 0 -0.58867455 0.073093891 0.5705139 0 0.073093891 -0.5705139 0 0.073093891;
	setAttr -s 48 ".ed[0:47]"  0 14 1 14 1 1 2 16 1 16 3 1 4 19 1 19 5 1
		 6 10 1 10 7 1 0 17 1 17 2 1 1 15 1 15 3 1 2 8 1 8 4 1 3 18 1 18 5 1 4 11 1 11 6 1
		 5 9 1 9 7 1 6 13 1 13 0 1 7 12 1 12 1 1 14 20 1 20 17 1 15 20 1 16 20 1 16 21 1 21 8 1
		 18 21 1 19 21 1 19 22 1 22 11 1 9 22 1 10 22 1 10 23 1 23 13 1 12 23 1 14 23 1 12 24 1
		 24 15 1 9 24 1 18 24 1 13 25 1 25 11 1 17 25 1 8 25 1;
	setAttr -s 24 -ch 96 ".fc[0:23]" -type "polyFaces" 
		f 4 0 24 25 -9
		mu 0 4 0 1 2 3
		f 4 1 10 26 -25
		mu 0 4 1 4 5 2
		f 4 -27 11 -4 27
		mu 0 4 2 5 6 7
		f 4 -26 -28 -3 -10
		mu 0 4 3 2 7 8
		f 4 2 28 29 -13
		mu 0 4 8 7 9 10
		f 4 3 14 30 -29
		mu 0 4 7 6 11 9
		f 4 -31 15 -6 31
		mu 0 4 9 11 12 13
		f 4 -30 -32 -5 -14
		mu 0 4 10 9 13 14
		f 4 4 32 33 -17
		mu 0 4 14 13 30 27
		f 4 5 18 34 -33
		mu 0 4 13 12 17 30
		f 4 -35 19 -8 35
		mu 0 4 15 28 18 19
		f 4 -34 -36 -7 -18
		mu 0 4 16 15 19 20
		f 4 6 36 37 -21
		mu 0 4 20 19 21 22
		f 4 7 22 38 -37
		mu 0 4 19 18 23 21
		f 4 -39 23 -2 39
		mu 0 4 21 23 4 1
		f 4 -38 -40 -1 -22
		mu 0 4 22 21 1 0
		f 4 -24 40 41 -11
		mu 0 4 4 23 29 5
		f 4 -23 -20 42 -41
		mu 0 4 23 18 28 29
		f 4 -43 -19 -16 43
		mu 0 4 24 17 12 11
		f 4 -42 -44 -15 -12
		mu 0 4 5 24 11 6
		f 4 20 44 45 17
		mu 0 4 20 22 26 16
		f 4 21 8 46 -45
		mu 0 4 22 0 3 26
		f 4 -47 9 12 47
		mu 0 4 25 3 8 10
		f 4 -46 -48 13 16
		mu 0 4 27 25 10 14;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 3 
		2 0 
		3 0 
		5 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "buddy_eye_l" -p "geometry_GRP";
	rename -uid "A1D5777F-49AC-E00A-C4E8-1B882E71ACA6";
	setAttr -l on ".tx";
	setAttr -l on ".ty";
	setAttr -l on ".tz";
	setAttr ".r" -type "double3" 0 14.999999999999998 0 ;
	setAttr -l on ".rx";
	setAttr -l on ".ry";
	setAttr -l on ".rz";
	setAttr -l on ".sx";
	setAttr -l on ".sy";
	setAttr -l on ".sz";
	setAttr ".rp" -type "double3" 4.9018560295517757 38.174055827991289 11.108806398564166 ;
	setAttr ".rpt" -type "double3" 2.7081439704482242 0 -1.6472170957444106 ;
	setAttr ".sp" -type "double3" 4.9018560295517757 38.174055827991289 11.108806398564166 ;
	setAttr ".it" no;
createNode mesh -n "buddy_eye_lShape" -p "buddy_eye_l";
	rename -uid "2320A225-4A87-3B2B-276C-63A7321C9082";
	setAttr -k off ".v";
	setAttr -s 4 ".iog[0].og";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".pv" -type "double2" 0.91999998688697815 0.080000001005828381 ;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
	setAttr ".dr" 3;
	setAttr ".dsm" 2;
	setAttr ".vcs" 2;
createNode mesh -n "buddy_eye_lShapeOrig" -p "buddy_eye_l";
	rename -uid "03420FAA-4883-EB85-BD11-E5B44D82EA14";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 439 ".uvst[0].uvsp";
	setAttr ".uvst[0].uvsp[0:249]" -type "float2" 0.8442232 0.011800935 0.85180092
		 0.011800935 0.85937858 0.011800935 0.86695623 0.011800935 0.87453389 0.011800935
		 0.88211161 0.011800935 0.88968921 0.011800935 0.89726692 0.011800935 0.90484464 0.011800935
		 0.9124223 0.011800935 0.91999996 0.011800935 0.92757767 0.011800935 0.93515533 0.011800935
		 0.94273299 0.011800935 0.95031065 0.011800935 0.95788836 0.011800935 0.96546602 0.011800935
		 0.97304374 0.011800935 0.9806214 0.011800935 0.988199 0.011800935 0.99577677 0.011800935
		 0.8442232 0.019378593 0.85180092 0.019378593 0.85937858 0.019378593 0.86695623 0.019378593
		 0.87453389 0.019378593 0.88211161 0.019378593 0.88968921 0.019378593 0.89726692 0.019378593
		 0.90484464 0.019378593 0.9124223 0.019378593 0.91999996 0.019378593 0.92757767 0.019378593
		 0.93515533 0.019378593 0.94273299 0.019378593 0.95031065 0.019378593 0.95788836 0.019378593
		 0.96546602 0.019378593 0.97304374 0.019378593 0.9806214 0.019378593 0.988199 0.019378593
		 0.99577677 0.019378593 0.8442232 0.026956288 0.85180092 0.026956288 0.85937858 0.026956288
		 0.86695623 0.026956288 0.87453389 0.026956288 0.88211161 0.026956288 0.88968921 0.026956288
		 0.89726692 0.026956288 0.90484464 0.026956288 0.9124223 0.026956288 0.91999996 0.026956288
		 0.92757767 0.026956288 0.93515533 0.026956288 0.94273299 0.026956288 0.95031065 0.026956288
		 0.95788836 0.026956288 0.96546602 0.026956288 0.97304374 0.026956288 0.9806214 0.026956288
		 0.988199 0.026956288 0.99577677 0.026956288 0.8442232 0.03453394 0.85180092 0.03453394
		 0.85937858 0.03453394 0.86695623 0.03453394 0.87453389 0.03453394 0.88211161 0.03453394
		 0.88968921 0.03453394 0.89726692 0.03453394 0.90484464 0.03453394 0.9124223 0.03453394
		 0.91999996 0.03453394 0.92757767 0.03453394 0.93515533 0.03453394 0.94273299 0.03453394
		 0.95031065 0.03453394 0.95788836 0.03453394 0.96546602 0.03453394 0.97304374 0.03453394
		 0.9806214 0.03453394 0.988199 0.03453394 0.99577677 0.03453394 0.8442232 0.042111628
		 0.85180092 0.042111628 0.85937858 0.042111628 0.86695623 0.042111628 0.87453389 0.042111628
		 0.88211161 0.042111628 0.88968921 0.042111628 0.89726692 0.042111628 0.90484464 0.042111628
		 0.9124223 0.042111628 0.91999996 0.042111628 0.92757767 0.042111628 0.93515533 0.042111628
		 0.94273299 0.042111628 0.95031065 0.042111628 0.95788836 0.042111628 0.96546602 0.042111628
		 0.97304374 0.042111628 0.9806214 0.042111628 0.988199 0.042111628 0.99577677 0.042111628
		 0.8442232 0.049689315 0.85180092 0.049689315 0.85937858 0.049689315 0.86695623 0.049689315
		 0.87453389 0.049689315 0.88211161 0.049689315 0.88968921 0.049689315 0.89726692 0.049689315
		 0.90484464 0.049689315 0.9124223 0.049689315 0.91999996 0.049689315 0.92757767 0.049689315
		 0.93515533 0.049689315 0.94273299 0.049689315 0.95031065 0.049689315 0.95788836 0.049689315
		 0.96546602 0.049689315 0.97304374 0.049689315 0.9806214 0.049689315 0.988199 0.049689315
		 0.99577677 0.049689315 0.8442232 0.057266966 0.85180092 0.057266966 0.85937858 0.057266966
		 0.86695623 0.057266966 0.87453389 0.057266966 0.88211161 0.057266966 0.88968921 0.057266966
		 0.89726692 0.057266966 0.90484464 0.057266966 0.9124223 0.057266966 0.91999996 0.057266966
		 0.92757767 0.057266966 0.93515533 0.057266966 0.94273299 0.057266966 0.95031065 0.057266966
		 0.95788836 0.057266966 0.96546602 0.057266966 0.97304374 0.057266966 0.9806214 0.057266966
		 0.988199 0.057266966 0.99577677 0.057266966 0.8442232 0.064844653 0.85180092 0.064844653
		 0.85937858 0.064844653 0.86695623 0.064844653 0.87453389 0.064844653 0.88211161 0.064844653
		 0.88968921 0.064844653 0.89726692 0.064844653 0.90484464 0.064844653 0.9124223 0.064844653
		 0.91999996 0.064844653 0.92757767 0.064844653 0.93515533 0.064844653 0.94273299 0.064844653
		 0.95031065 0.064844653 0.95788836 0.064844653 0.96546602 0.064844653 0.97304374 0.064844653
		 0.9806214 0.064844653 0.988199 0.064844653 0.99577677 0.064844653 0.8442232 0.072422341
		 0.85180092 0.072422341 0.85937858 0.072422341 0.86695623 0.072422341 0.87453389 0.072422341
		 0.88211161 0.072422341 0.88968921 0.072422341 0.89726692 0.072422341 0.90484464 0.072422341
		 0.9124223 0.072422341 0.91999996 0.072422341 0.92757767 0.072422341 0.93515533 0.072422341
		 0.94273299 0.072422341 0.95031065 0.072422341 0.95788836 0.072422341 0.96546602 0.072422341
		 0.97304374 0.072422341 0.9806214 0.072422341 0.988199 0.072422341 0.99577677 0.072422341
		 0.8442232 0.079999998 0.85180092 0.079999998 0.85937858 0.079999998 0.86695623 0.079999998
		 0.87453389 0.079999998 0.88211161 0.079999998 0.88968921 0.079999998 0.89726692 0.079999998
		 0.90484464 0.079999998 0.9124223 0.079999998 0.91999996 0.079999998 0.92757767 0.079999998
		 0.93515533 0.079999998 0.94273299 0.079999998 0.95031065 0.079999998 0.95788836 0.079999998
		 0.96546602 0.079999998 0.97304374 0.079999998 0.9806214 0.079999998 0.988199 0.079999998
		 0.99577677 0.079999998 0.8442232 0.087577686 0.85180092 0.087577686 0.85937858 0.087577686
		 0.86695623 0.087577686 0.87453389 0.087577686 0.88211161 0.087577686 0.88968921 0.087577686
		 0.89726692 0.087577686 0.90484464 0.087577686 0.9124223 0.087577686 0.91999996 0.087577686
		 0.92757767 0.087577686 0.93515533 0.087577686 0.94273299 0.087577686 0.95031065 0.087577686
		 0.95788836 0.087577686 0.96546602 0.087577686 0.97304374 0.087577686 0.9806214 0.087577686
		 0.988199 0.087577686 0.99577677 0.087577686 0.8442232 0.095155373 0.85180092 0.095155373
		 0.85937858 0.095155373 0.86695623 0.095155373 0.87453389 0.095155373 0.88211161 0.095155373
		 0.88968921 0.095155373 0.89726692 0.095155373 0.90484464 0.095155373 0.9124223 0.095155373
		 0.91999996 0.095155373 0.92757767 0.095155373 0.93515533 0.095155373 0.94273299 0.095155373
		 0.95031065 0.095155373 0.95788836 0.095155373 0.96546602 0.095155373 0.97304374 0.095155373
		 0.9806214 0.095155373;
	setAttr ".uvst[0].uvsp[250:438]" 0.988199 0.095155373 0.99577677 0.095155373
		 0.8442232 0.10273306 0.85180092 0.10273306 0.85937858 0.10273306 0.86695623 0.10273306
		 0.87453389 0.10273306 0.88211161 0.10273306 0.88968921 0.10273306 0.89726692 0.10273306
		 0.90484464 0.10273306 0.9124223 0.10273306 0.91999996 0.10273306 0.92757767 0.10273306
		 0.93515533 0.10273306 0.94273299 0.10273306 0.95031065 0.10273306 0.95788836 0.10273306
		 0.96546602 0.10273306 0.97304374 0.10273306 0.9806214 0.10273306 0.988199 0.10273306
		 0.99577677 0.10273306 0.8442232 0.11031069 0.85180092 0.11031069 0.85937858 0.11031069
		 0.86695623 0.11031069 0.87453389 0.11031069 0.88211161 0.11031069 0.88968921 0.11031069
		 0.89726692 0.11031069 0.90484464 0.11031069 0.9124223 0.11031069 0.91999996 0.11031069
		 0.92757767 0.11031069 0.93515533 0.11031069 0.94273299 0.11031069 0.95031065 0.11031069
		 0.95788836 0.11031069 0.96546602 0.11031069 0.97304374 0.11031069 0.9806214 0.11031069
		 0.988199 0.11031069 0.99577677 0.11031069 0.8442232 0.11788839 0.85180092 0.11788839
		 0.85937858 0.11788839 0.86695623 0.11788839 0.87453389 0.11788839 0.88211161 0.11788839
		 0.88968921 0.11788839 0.89726692 0.11788839 0.90484464 0.11788839 0.9124223 0.11788839
		 0.91999996 0.11788839 0.92757767 0.11788839 0.93515533 0.11788839 0.94273299 0.11788839
		 0.95031065 0.11788839 0.95788836 0.11788839 0.96546602 0.11788839 0.97304374 0.11788839
		 0.9806214 0.11788839 0.988199 0.11788839 0.99577677 0.11788839 0.8442232 0.12546606
		 0.85180092 0.12546606 0.85937858 0.12546606 0.86695623 0.12546606 0.87453389 0.12546606
		 0.88211161 0.12546606 0.88968921 0.12546606 0.89726692 0.12546606 0.90484464 0.12546606
		 0.9124223 0.12546606 0.91999996 0.12546606 0.92757767 0.12546606 0.93515533 0.12546606
		 0.94273299 0.12546606 0.95031065 0.12546606 0.95788836 0.12546606 0.96546602 0.12546606
		 0.97304374 0.12546606 0.9806214 0.12546606 0.988199 0.12546606 0.99577677 0.12546606
		 0.8442232 0.13304375 0.85180092 0.13304375 0.85937858 0.13304375 0.86695623 0.13304375
		 0.87453389 0.13304375 0.88211161 0.13304375 0.88968921 0.13304375 0.89726692 0.13304375
		 0.90484464 0.13304375 0.9124223 0.13304375 0.91999996 0.13304375 0.92757767 0.13304375
		 0.93515533 0.13304375 0.94273299 0.13304375 0.95031065 0.13304375 0.95788836 0.13304375
		 0.96546602 0.13304375 0.97304374 0.13304375 0.9806214 0.13304375 0.988199 0.13304375
		 0.99577677 0.13304375 0.8442232 0.14062144 0.85180092 0.14062144 0.85937858 0.14062144
		 0.86695623 0.14062144 0.87453389 0.14062144 0.88211161 0.14062144 0.88968921 0.14062144
		 0.89726692 0.14062144 0.90484464 0.14062144 0.9124223 0.14062144 0.91999996 0.14062144
		 0.92757767 0.14062144 0.93515533 0.14062144 0.94273299 0.14062144 0.95031065 0.14062144
		 0.95788836 0.14062144 0.96546602 0.14062144 0.97304374 0.14062144 0.9806214 0.14062144
		 0.988199 0.14062144 0.99577677 0.14062144 0.8442232 0.14819905 0.85180092 0.14819905
		 0.85937858 0.14819905 0.86695623 0.14819905 0.87453389 0.14819905 0.88211161 0.14819905
		 0.88968921 0.14819905 0.89726692 0.14819905 0.90484464 0.14819905 0.9124223 0.14819905
		 0.91999996 0.14819905 0.92757767 0.14819905 0.93515533 0.14819905 0.94273299 0.14819905
		 0.95031065 0.14819905 0.95788836 0.14819905 0.96546602 0.14819905 0.97304374 0.14819905
		 0.9806214 0.14819905 0.988199 0.14819905 0.99577677 0.14819905 0.84801203 0.004223248
		 0.85558969 0.004223248 0.86316741 0.004223248 0.87074506 0.004223248 0.87832272 0.004223248
		 0.88590044 0.004223248 0.8934781 0.004223248 0.90105575 0.004223248 0.90863341 0.004223248
		 0.91621113 0.004223248 0.92378879 0.004223248 0.93136644 0.004223248 0.9389441 0.004223248
		 0.94652182 0.004223248 0.95409954 0.004223248 0.96167719 0.004223248 0.96925485 0.004223248
		 0.97683251 0.004223248 0.98441017 0.004223248 0.99198788 0.004223248 0.84801203 0.15577675
		 0.85558969 0.15577675 0.86316741 0.15577675 0.87074506 0.15577675 0.87832272 0.15577675
		 0.88590044 0.15577675 0.8934781 0.15577675 0.90105575 0.15577675 0.90863341 0.15577675
		 0.91621113 0.15577675 0.92378879 0.15577675 0.93136644 0.15577675 0.9389441 0.15577675
		 0.94652182 0.15577675 0.95409954 0.15577675 0.96167719 0.15577675 0.96925485 0.15577675
		 0.97683251 0.15577675 0.98441017 0.15577675 0.99198788 0.15577675;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 382 ".pt";
	setAttr ".pt[0:165]" -type "float3"  4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 
		38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807;
	setAttr ".pt[166:331]" 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807;
	setAttr ".pt[332:381]" 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 
		4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807 4.9018559 38.174057 11.108807;
	setAttr -s 382 ".vt";
	setAttr ".vt[0:165]"  1.33869112 0.4349671 -8.88712406 1.13875866 0.82735658 -8.88712406
		 0.82735658 1.13875866 -8.88712406 0.43496704 1.338691 -8.88712406 -8.8817842e-16 1.40758288 -8.88712406
		 -0.43496704 1.33869088 -8.88712406 -0.8273564 1.13875842 -8.88712406 -1.1387583 0.82735628 -8.88712406
		 -1.33869064 0.43496695 -8.88712406 -1.40758264 3.9466759e-15 -8.88712406 -1.33869064 -0.43496695 -8.88712406
		 -1.1387583 -0.82735622 -8.88712406 -0.82735622 -1.13875806 -8.88712406 -0.43496695 -1.3386904 -8.88712406
		 -4.1949221e-08 -1.4075824 -8.88712406 0.4349668 -1.33869028 -8.88712406 0.8273561 -1.13875794 -8.88712406
		 1.13875794 -0.82735616 -8.88712406 1.33869028 -0.43496683 -8.88712406 1.40758228 3.9466759e-15 -8.88712406
		 2.64441919 0.85922384 -8.55751419 2.24947739 1.63434076 -8.55751419 1.63434076 2.24947715 -8.55751419
		 0.85922372 2.64441895 -8.55751419 -8.8817842e-16 2.78050637 -8.55751419 -0.85922372 2.64441872 -8.55751419
		 -1.63434041 2.24947643 -8.55751419 -2.24947643 1.63434029 -8.55751419 -2.644418 0.85922348 -8.55751419
		 -2.78050542 3.8002997e-15 -8.55751419 -2.644418 -0.85922348 -8.55751419 -2.24947619 -1.63434017 -8.55751419
		 -1.63434017 -2.24947619 -8.55751419 -0.85922348 -2.64441776 -8.55751419 -8.2865505e-08 -2.78050518 -8.55751419
		 0.85922325 -2.64441752 -8.55751419 1.63433981 -2.24947596 -8.55751419 2.24947572 -1.63433993 -8.55751419
		 2.64441752 -0.85922331 -8.55751419 2.78050494 3.8002997e-15 -8.55751419 3.88503289 1.26232362 -8.01718998
		 3.30480647 2.40108228 -8.01718998 2.40108228 3.30480623 -8.01718998 1.2623235 3.88503218 -8.01718998
		 -8.8817842e-16 4.084964275 -8.01718998 -1.2623235 3.88503194 -8.01718998 -2.4010818 3.30480528 -8.01718998
		 -3.30480504 2.40108156 -8.01718998 -3.88503146 1.26232314 -8.01718998 -4.084963799 3.5603476e-15 -8.01718998
		 -3.88503146 -1.26232314 -8.01718998 -3.3048048 -2.40108132 -8.01718998 -2.40108132 -3.3048048 -8.01718998
		 -1.26232314 -3.88503098 -8.01718998 -1.2174137e-07 -4.084963322 -8.01718998 1.26232278 -3.88503075 -8.01718998
		 2.40108061 -3.30480456 -8.01718998 3.30480433 -2.40108085 -8.01718998 3.88503051 -1.26232302 -8.01718998
		 4.084962368 3.5603476e-15 -8.01718998 5.029983997 1.63434076 -7.27945662 4.27875996 3.10870075 -7.27945662
		 3.10870075 4.27875948 -7.27945662 1.63434064 5.029983521 -7.27945662 -8.8817842e-16 5.28883743 -7.27945662
		 -1.63434064 5.029983044 -7.27945662 -3.10870028 4.27875853 -7.27945662 -4.27875805 3.1086998 -7.27945662
		 -5.029981613 1.63434005 -7.27945662 -5.28883553 3.2327281e-15 -7.27945662 -5.029981613 -1.63434005 -7.27945662
		 -4.27875805 -3.10869956 -7.27945662 -3.10869956 -4.27875757 -7.27945662 -1.63434005 -5.029981136 -7.27945662
		 -1.5761955e-07 -5.28883505 -7.27945662 1.63433969 -5.029981136 -7.27945662 3.10869884 -4.27875757 -7.27945662
		 4.2787571 -3.10869932 -7.27945662 5.029980659 -1.63433981 -7.27945662 5.28883457 3.2327281e-15 -7.27945662
		 6.051080227 1.966115 -6.36247826 5.14735651 3.73977304 -6.36247826 3.73977304 5.14735556 -6.36247826
		 1.96611488 6.05107975 -6.36247826 -8.8817842e-16 6.36248112 -6.36247826 -1.96611488 6.051079273 -6.36247826
		 -3.73977232 5.1473546 -6.36247826 -5.14735413 3.73977184 -6.36247826 -6.051077843 1.96611416 -6.36247826
		 -6.36247969 2.8255079e-15 -6.36247826 -6.051077843 -1.96611416 -6.36247826 -5.14735413 -3.73977137 -6.36247826
		 -3.73977137 -5.14735365 -6.36247826 -1.96611416 -6.051076889 -6.36247826 -1.8961663e-07 -6.36247873 -6.36247826
		 1.96611369 -6.051076889 -6.36247826 3.73977065 -5.14735317 -6.36247826 5.14735317 -3.73977113 -6.36247826
		 6.051076412 -1.96611392 -6.36247826 6.36247826 2.8255079e-15 -6.36247826 6.92317915 2.24947715 -5.28883457
		 5.88920784 4.27875948 -5.28883457 4.27875948 5.88920736 -5.28883457 2.24947667 6.92317772 -5.28883457
		 -8.8817842e-16 7.27945948 -5.28883457 -2.24947667 6.92317724 -5.28883457 -4.278759 5.88920641 -5.28883457
		 -5.88920593 4.27875805 -5.28883457 -6.92317629 2.24947619 -5.28883457 -7.27945805 2.3487144e-15 -5.28883457
		 -6.92317629 -2.24947619 -5.28883457 -5.88920546 -4.27875805 -5.28883457 -4.27875805 -5.8892045 -5.28883457
		 -2.24947619 -6.92317533 -5.28883457 -2.1694471e-07 -7.27945757 -5.28883457 2.24947548 -6.92317533 -5.28883457
		 4.2787571 -5.88920403 -5.28883457 5.88920403 -4.27875757 -5.28883457 6.92317486 -2.24947572 -5.28883457
		 7.27945662 2.3487144e-15 -5.28883457 7.62480593 2.47744942 -4.084962368 6.48604774 4.71238899 -4.084962368
		 4.71238899 6.48604679 -4.084962368 2.47744918 7.62480497 -4.084962368 -8.8817842e-16 8.017193794 -4.084962368
		 -2.47744918 7.62480497 -4.084962368 -4.71238804 6.48604536 -4.084962368 -6.48604488 4.71238708 -4.084962368
		 -7.62480307 2.47744846 -4.084962368 -8.017191887 1.8140877e-15 -4.084962368 -7.62480307 -2.47744846 -4.084962368
		 -6.48604488 -4.71238708 -4.084962368 -4.71238708 -6.48604441 -4.084962368 -2.47744846 -7.62480211 -4.084962368
		 -2.3893088e-07 -8.017190933 -4.084962368 2.47744799 -7.62480164 -4.084962368 4.71238613 -6.48604393 -4.084962368
		 6.48604345 -4.71238661 -4.084962368 7.62480116 -2.47744823 -4.084962368 8.01718998 1.8140877e-15 -4.084962368
		 8.13868523 2.64441895 -2.7805047 6.92317915 5.029983997 -2.7805047 5.029983997 6.92317867 -2.7805047
		 2.64441872 8.13868427 -2.7805047 -8.8817842e-16 8.55751896 -2.7805047 -2.64441872 8.13868332 -2.7805047
		 -5.029983044 6.92317724 -2.7805047 -6.92317677 5.02998209 -2.7805047 -8.13868237 2.644418 -2.7805047
		 -8.5575161 1.2347921e-15 -2.7805047 -8.13868237 -2.644418 -2.7805047 -6.92317629 -5.029981613 -2.7805047
		 -5.029981613 -6.92317581 -2.7805047 -2.644418 -8.13868141 -2.7805047 -2.550338e-07 -8.55751514 -2.7805047
		 2.64441729 -8.13868046 -2.7805047 5.029980659 -6.92317533 -2.7805047 6.92317486 -5.029981136 -2.7805047
		 8.1386795 -2.64441752 -2.7805047 8.55751419 1.2347921e-15 -2.7805047 8.45216274 2.74627399 -1.40758133
		 7.18983889 5.22372341 -1.40758133 5.22372341 7.18983841 -1.40758133 2.74627376 8.45216179 -1.40758133
		 -8.8817842e-16 8.88712788 -1.40758133 -2.74627376 8.45216084 -1.40758133;
	setAttr ".vt[166:331]" -5.22372246 7.18983698 -1.40758133 -7.18983603 5.22372198 -1.40758133
		 -8.45215893 2.74627304 -1.40758133 -8.88712597 6.2509168e-16 -1.40758133 -8.45215893 -2.74627304 -1.40758133
		 -7.18983555 -5.2237215 -1.40758133 -5.2237215 -7.18983507 -1.40758133 -2.74627304 -8.45215797 -1.40758133
		 -2.6485694e-07 -8.88712502 -1.40758133 2.74627209 -8.45215797 -1.40758133 5.22372007 -7.18983459 -1.40758133
		 7.18983412 -5.22372103 -1.40758133 8.45215797 -2.74627233 -1.40758133 8.88712406 6.2509168e-16 -1.40758133
		 8.55751991 2.78050661 -2.3179207e-15 7.27946138 5.28883791 -1.2039979e-15 5.28883791 7.27946091 -3.1998374e-16
		 2.78050637 8.55751896 2.4758794e-16 -8.8817842e-16 8.99790668 4.4315959e-16 -2.78050637 8.55751801 2.4758752e-16
		 -5.28883648 7.279459 -3.1998448e-16 -7.27945852 5.288836 -1.2039986e-15 -8.5575161 2.78050542 -2.3179213e-15
		 -8.99790478 7.8886091e-31 -3.5527137e-15 -8.5575161 -2.78050542 -4.787506e-15 -7.27945805 -5.28883553 -5.9014285e-15
		 -5.28883553 -7.27945757 -6.785442e-15 -2.78050542 -8.55751514 -7.3530142e-15 -2.681584e-07 -8.99790382 -7.5485857e-15
		 2.7805047 -8.55751514 -7.3530142e-15 5.28883457 -7.27945709 -6.785442e-15 7.27945662 -5.28883505 -5.9014285e-15
		 8.55751419 -2.78050494 -4.7875056e-15 8.99790287 7.8886091e-31 -3.5527137e-15 8.45216274 2.74627399 1.40758133
		 7.18983889 5.22372341 1.40758133 5.22372341 7.18983841 1.40758133 2.74627376 8.45216179 1.40758133
		 -8.8817842e-16 8.88712788 1.40758133 -2.74627376 8.45216084 1.40758133 -5.22372246 7.18983698 1.40758133
		 -7.18983603 5.22372198 1.40758133 -8.45215893 2.74627304 1.40758133 -8.88712597 -6.2509168e-16 1.40758133
		 -8.45215893 -2.74627304 1.40758133 -7.18983555 -5.2237215 1.40758133 -5.2237215 -7.18983507 1.40758133
		 -2.74627304 -8.45215797 1.40758133 -2.6485694e-07 -8.88712502 1.40758133 2.74627209 -8.45215797 1.40758133
		 5.22372007 -7.18983459 1.40758133 7.18983412 -5.22372103 1.40758133 8.45215797 -2.74627233 1.40758133
		 8.88712406 -6.2509168e-16 1.40758133 8.13868523 2.64441895 2.7805047 6.92317915 5.029983997 2.7805047
		 5.029983997 6.92317867 2.7805047 2.64441872 8.13868427 2.7805047 -8.8817842e-16 8.55751896 2.7805047
		 -2.64441872 8.13868332 2.7805047 -5.029983044 6.92317724 2.7805047 -6.92317677 5.02998209 2.7805047
		 -8.13868237 2.644418 2.7805047 -8.5575161 -1.2347921e-15 2.7805047 -8.13868237 -2.644418 2.7805047
		 -6.92317629 -5.029981613 2.7805047 -5.029981613 -6.92317581 2.7805047 -2.644418 -8.13868141 2.7805047
		 -2.550338e-07 -8.55751514 2.7805047 2.64441729 -8.13868046 2.7805047 5.029980659 -6.92317533 2.7805047
		 6.92317486 -5.029981136 2.7805047 8.1386795 -2.64441752 2.7805047 8.55751419 -1.2347921e-15 2.7805047
		 7.62480593 2.47744942 4.084962368 6.48604774 4.71238899 4.084962368 4.71238899 6.48604679 4.084962368
		 2.47744918 7.62480497 4.084962368 -8.8817842e-16 8.017193794 4.084962368 -2.47744918 7.62480497 4.084962368
		 -4.71238804 6.48604536 4.084962368 -6.48604488 4.71238708 4.084962368 -7.62480307 2.47744846 4.084962368
		 -8.017191887 -1.8140877e-15 4.084962368 -7.62480307 -2.47744846 4.084962368 -6.48604488 -4.71238708 4.084962368
		 -4.71238708 -6.48604441 4.084962368 -2.47744846 -7.62480211 4.084962368 -2.3893088e-07 -8.017190933 4.084962368
		 2.47744799 -7.62480164 4.084962368 4.71238613 -6.48604393 4.084962368 6.48604345 -4.71238661 4.084962368
		 7.62480116 -2.47744823 4.084962368 8.01718998 -1.8140877e-15 4.084962368 6.92317915 2.24947715 5.28883457
		 5.88920784 4.27875948 5.28883457 4.27875948 5.88920736 5.28883457 2.24947667 6.92317772 5.28883457
		 -8.8817842e-16 7.27945948 5.28883457 -2.24947667 6.92317724 5.28883457 -4.278759 5.88920641 5.28883457
		 -5.88920593 4.27875805 5.28883457 -6.92317629 2.24947619 5.28883457 -7.27945805 -2.3487144e-15 5.28883457
		 -6.92317629 -2.24947619 5.28883457 -5.88920546 -4.27875805 5.28883457 -4.27875805 -5.8892045 5.28883457
		 -2.24947619 -6.92317533 5.28883457 -2.1694471e-07 -7.27945757 5.28883457 2.24947548 -6.92317533 5.28883457
		 4.2787571 -5.88920403 5.28883457 5.88920403 -4.27875757 5.28883457 6.92317486 -2.24947572 5.28883457
		 7.27945662 -2.3487144e-15 5.28883457 6.051080227 1.966115 6.36247826 5.14735651 3.73977304 6.36247826
		 3.73977304 5.14735556 6.36247826 1.96611488 6.05107975 6.36247826 -8.8817842e-16 6.36248112 6.36247826
		 -1.96611488 6.051079273 6.36247826 -3.73977232 5.1473546 6.36247826 -5.14735413 3.73977184 6.36247826
		 -6.051077843 1.96611416 6.36247826 -6.36247969 -2.8255079e-15 6.36247826 -6.051077843 -1.96611416 6.36247826
		 -5.14735413 -3.73977137 6.36247826 -3.73977137 -5.14735365 6.36247826 -1.96611416 -6.051076889 6.36247826
		 -1.8961663e-07 -6.36247873 6.36247826 1.96611369 -6.051076889 6.36247826 3.73977065 -5.14735317 6.36247826
		 5.14735317 -3.73977113 6.36247826 6.051076412 -1.96611392 6.36247826 6.36247826 -2.8255079e-15 6.36247826
		 5.029983997 1.63434076 7.27945662 4.27875996 3.10870075 7.27945662 3.10870075 4.27875948 7.27945662
		 1.63434064 5.029983521 7.27945662 -8.8817842e-16 5.28883743 7.27945662 -1.63434064 5.029983044 7.27945662
		 -3.10870028 4.27875853 7.27945662 -4.27875805 3.1086998 7.27945662 -5.029981613 1.63434005 7.27945662
		 -5.28883553 -3.2327281e-15 7.27945662 -5.029981613 -1.63434005 7.27945662 -4.27875805 -3.10869956 7.27945662
		 -3.10869956 -4.27875757 7.27945662 -1.63434005 -5.029981136 7.27945662 -1.5761955e-07 -5.28883505 7.27945662
		 1.63433969 -5.029981136 7.27945662 3.10869884 -4.27875757 7.27945662 4.2787571 -3.10869932 7.27945662
		 5.029980659 -1.63433981 7.27945662 5.28883457 -3.2327281e-15 7.27945662 3.88503289 1.26232362 8.01718998
		 3.30480647 2.40108228 8.01718998 2.40108228 3.30480623 8.01718998 1.2623235 3.88503218 8.01718998
		 -8.8817842e-16 4.084964275 8.01718998 -1.2623235 3.88503194 8.01718998 -2.4010818 3.30480528 8.01718998
		 -3.30480504 2.40108156 8.01718998 -3.88503146 1.26232314 8.01718998 -4.084963799 -3.5603476e-15 8.01718998
		 -3.88503146 -1.26232314 8.01718998 -3.3048048 -2.40108132 8.01718998;
	setAttr ".vt[332:381]" -2.40108132 -3.3048048 8.01718998 -1.26232314 -3.88503098 8.01718998
		 -1.2174137e-07 -4.084963322 8.01718998 1.26232278 -3.88503075 8.01718998 2.40108061 -3.30480456 8.01718998
		 3.30480433 -2.40108085 8.01718998 3.88503051 -1.26232302 8.01718998 4.084962368 -3.5603476e-15 8.01718998
		 2.64441919 0.85922384 8.55751419 2.24947739 1.63434076 8.55751419 1.63434076 2.24947715 8.55751419
		 0.85922372 2.64441895 8.55751419 -8.8817842e-16 2.78050637 8.55751419 -0.85922372 2.64441872 8.55751419
		 -1.63434041 2.24947643 8.55751419 -2.24947643 1.63434029 8.55751419 -2.644418 0.85922348 8.55751419
		 -2.78050542 -3.8002997e-15 8.55751419 -2.644418 -0.85922348 8.55751419 -2.24947619 -1.63434017 8.55751419
		 -1.63434017 -2.24947619 8.55751419 -0.85922348 -2.64441776 8.55751419 -8.2865505e-08 -2.78050518 8.55751419
		 0.85922325 -2.64441752 8.55751419 1.63433981 -2.24947596 8.55751419 2.24947572 -1.63433993 8.55751419
		 2.64441752 -0.85922331 8.55751419 2.78050494 -3.8002997e-15 8.55751419 1.33869112 0.4349671 8.88712406
		 1.13875866 0.82735658 8.88712406 0.82735658 1.13875866 8.88712406 0.43496704 1.338691 8.88712406
		 -8.8817842e-16 1.40758288 8.88712406 -0.43496704 1.33869088 8.88712406 -0.8273564 1.13875842 8.88712406
		 -1.1387583 0.82735628 8.88712406 -1.33869064 0.43496695 8.88712406 -1.40758264 -3.9466759e-15 8.88712406
		 -1.33869064 -0.43496695 8.88712406 -1.1387583 -0.82735622 8.88712406 -0.82735622 -1.13875806 8.88712406
		 -0.43496695 -1.3386904 8.88712406 -4.1949221e-08 -1.4075824 8.88712406 0.4349668 -1.33869028 8.88712406
		 0.8273561 -1.13875794 8.88712406 1.13875794 -0.82735616 8.88712406 1.33869028 -0.43496683 8.88712406
		 1.40758228 -3.9466759e-15 8.88712406 -8.8817842e-16 3.9958716e-15 -8.99790287 -8.8817842e-16 -3.9958716e-15 8.99790287;
	setAttr -s 780 ".ed";
	setAttr ".ed[0:165]"  0 1 1 1 2 1 2 3 1 3 4 1 4 5 1 5 6 1 6 7 1 7 8 1 8 9 1
		 9 10 1 10 11 1 11 12 1 12 13 1 13 14 1 14 15 1 15 16 1 16 17 1 17 18 1 18 19 1 19 0 1
		 20 21 1 21 22 1 22 23 1 23 24 1 24 25 1 25 26 1 26 27 1 27 28 1 28 29 1 29 30 1 30 31 1
		 31 32 1 32 33 1 33 34 1 34 35 1 35 36 1 36 37 1 37 38 1 38 39 1 39 20 1 40 41 1 41 42 1
		 42 43 1 43 44 1 44 45 1 45 46 1 46 47 1 47 48 1 48 49 1 49 50 1 50 51 1 51 52 1 52 53 1
		 53 54 1 54 55 1 55 56 1 56 57 1 57 58 1 58 59 1 59 40 1 60 61 1 61 62 1 62 63 1 63 64 1
		 64 65 1 65 66 1 66 67 1 67 68 1 68 69 1 69 70 1 70 71 1 71 72 1 72 73 1 73 74 1 74 75 1
		 75 76 1 76 77 1 77 78 1 78 79 1 79 60 1 80 81 1 81 82 1 82 83 1 83 84 1 84 85 1 85 86 1
		 86 87 1 87 88 1 88 89 1 89 90 1 90 91 1 91 92 1 92 93 1 93 94 1 94 95 1 95 96 1 96 97 1
		 97 98 1 98 99 1 99 80 1 100 101 1 101 102 1 102 103 1 103 104 1 104 105 1 105 106 1
		 106 107 1 107 108 1 108 109 1 109 110 1 110 111 1 111 112 1 112 113 1 113 114 1 114 115 1
		 115 116 1 116 117 1 117 118 1 118 119 1 119 100 1 120 121 1 121 122 1 122 123 1 123 124 1
		 124 125 1 125 126 1 126 127 1 127 128 1 128 129 1 129 130 1 130 131 1 131 132 1 132 133 1
		 133 134 1 134 135 1 135 136 1 136 137 1 137 138 1 138 139 1 139 120 1 140 141 1 141 142 1
		 142 143 1 143 144 1 144 145 1 145 146 1 146 147 1 147 148 1 148 149 1 149 150 1 150 151 1
		 151 152 1 152 153 1 153 154 1 154 155 1 155 156 1 156 157 1 157 158 1 158 159 1 159 140 1
		 160 161 1 161 162 1 162 163 1 163 164 1 164 165 1 165 166 1;
	setAttr ".ed[166:331]" 166 167 1 167 168 1 168 169 1 169 170 1 170 171 1 171 172 1
		 172 173 1 173 174 1 174 175 1 175 176 1 176 177 1 177 178 1 178 179 1 179 160 1 180 181 1
		 181 182 1 182 183 1 183 184 1 184 185 1 185 186 1 186 187 1 187 188 1 188 189 1 189 190 1
		 190 191 1 191 192 1 192 193 1 193 194 1 194 195 1 195 196 1 196 197 1 197 198 1 198 199 1
		 199 180 1 200 201 1 201 202 1 202 203 1 203 204 1 204 205 1 205 206 1 206 207 1 207 208 1
		 208 209 1 209 210 1 210 211 1 211 212 1 212 213 1 213 214 1 214 215 1 215 216 1 216 217 1
		 217 218 1 218 219 1 219 200 1 220 221 1 221 222 1 222 223 1 223 224 1 224 225 1 225 226 1
		 226 227 1 227 228 1 228 229 1 229 230 1 230 231 1 231 232 1 232 233 1 233 234 1 234 235 1
		 235 236 1 236 237 1 237 238 1 238 239 1 239 220 1 240 241 1 241 242 1 242 243 1 243 244 1
		 244 245 1 245 246 1 246 247 1 247 248 1 248 249 1 249 250 1 250 251 1 251 252 1 252 253 1
		 253 254 1 254 255 1 255 256 1 256 257 1 257 258 1 258 259 1 259 240 1 260 261 1 261 262 1
		 262 263 1 263 264 1 264 265 1 265 266 1 266 267 1 267 268 1 268 269 1 269 270 1 270 271 1
		 271 272 1 272 273 1 273 274 1 274 275 1 275 276 1 276 277 1 277 278 1 278 279 1 279 260 1
		 280 281 1 281 282 1 282 283 1 283 284 1 284 285 1 285 286 1 286 287 1 287 288 1 288 289 1
		 289 290 1 290 291 1 291 292 1 292 293 1 293 294 1 294 295 1 295 296 1 296 297 1 297 298 1
		 298 299 1 299 280 1 300 301 1 301 302 1 302 303 1 303 304 1 304 305 1 305 306 1 306 307 1
		 307 308 1 308 309 1 309 310 1 310 311 1 311 312 1 312 313 1 313 314 1 314 315 1 315 316 1
		 316 317 1 317 318 1 318 319 1 319 300 1 320 321 1 321 322 1 322 323 1 323 324 1 324 325 1
		 325 326 1 326 327 1 327 328 1 328 329 1 329 330 1 330 331 1 331 332 1;
	setAttr ".ed[332:497]" 332 333 1 333 334 1 334 335 1 335 336 1 336 337 1 337 338 1
		 338 339 1 339 320 1 340 341 1 341 342 1 342 343 1 343 344 1 344 345 1 345 346 1 346 347 1
		 347 348 1 348 349 1 349 350 1 350 351 1 351 352 1 352 353 1 353 354 1 354 355 1 355 356 1
		 356 357 1 357 358 1 358 359 1 359 340 1 360 361 1 361 362 1 362 363 1 363 364 1 364 365 1
		 365 366 1 366 367 1 367 368 1 368 369 1 369 370 1 370 371 1 371 372 1 372 373 1 373 374 1
		 374 375 1 375 376 1 376 377 1 377 378 1 378 379 1 379 360 1 0 20 1 1 21 1 2 22 1
		 3 23 1 4 24 1 5 25 1 6 26 1 7 27 1 8 28 1 9 29 1 10 30 1 11 31 1 12 32 1 13 33 1
		 14 34 1 15 35 1 16 36 1 17 37 1 18 38 1 19 39 1 20 40 1 21 41 1 22 42 1 23 43 1 24 44 1
		 25 45 1 26 46 1 27 47 1 28 48 1 29 49 1 30 50 1 31 51 1 32 52 1 33 53 1 34 54 1 35 55 1
		 36 56 1 37 57 1 38 58 1 39 59 1 40 60 1 41 61 1 42 62 1 43 63 1 44 64 1 45 65 1 46 66 1
		 47 67 1 48 68 1 49 69 1 50 70 1 51 71 1 52 72 1 53 73 1 54 74 1 55 75 1 56 76 1 57 77 1
		 58 78 1 59 79 1 60 80 1 61 81 1 62 82 1 63 83 1 64 84 1 65 85 1 66 86 1 67 87 1 68 88 1
		 69 89 1 70 90 1 71 91 1 72 92 1 73 93 1 74 94 1 75 95 1 76 96 1 77 97 1 78 98 1 79 99 1
		 80 100 1 81 101 1 82 102 1 83 103 1 84 104 1 85 105 1 86 106 1 87 107 1 88 108 1
		 89 109 1 90 110 1 91 111 1 92 112 1 93 113 1 94 114 1 95 115 1 96 116 1 97 117 1
		 98 118 1 99 119 1 100 120 1 101 121 1 102 122 1 103 123 1 104 124 1 105 125 1 106 126 1
		 107 127 1 108 128 1 109 129 1 110 130 1 111 131 1 112 132 1 113 133 1 114 134 1 115 135 1
		 116 136 1 117 137 1;
	setAttr ".ed[498:663]" 118 138 1 119 139 1 120 140 1 121 141 1 122 142 1 123 143 1
		 124 144 1 125 145 1 126 146 1 127 147 1 128 148 1 129 149 1 130 150 1 131 151 1 132 152 1
		 133 153 1 134 154 1 135 155 1 136 156 1 137 157 1 138 158 1 139 159 1 140 160 1 141 161 1
		 142 162 1 143 163 1 144 164 1 145 165 1 146 166 1 147 167 1 148 168 1 149 169 1 150 170 1
		 151 171 1 152 172 1 153 173 1 154 174 1 155 175 1 156 176 1 157 177 1 158 178 1 159 179 1
		 160 180 1 161 181 1 162 182 1 163 183 1 164 184 1 165 185 1 166 186 1 167 187 1 168 188 1
		 169 189 1 170 190 1 171 191 1 172 192 1 173 193 1 174 194 1 175 195 1 176 196 1 177 197 1
		 178 198 1 179 199 1 180 200 1 181 201 1 182 202 1 183 203 1 184 204 1 185 205 1 186 206 1
		 187 207 1 188 208 1 189 209 1 190 210 1 191 211 1 192 212 1 193 213 1 194 214 1 195 215 1
		 196 216 1 197 217 1 198 218 1 199 219 1 200 220 1 201 221 1 202 222 1 203 223 1 204 224 1
		 205 225 1 206 226 1 207 227 1 208 228 1 209 229 1 210 230 1 211 231 1 212 232 1 213 233 1
		 214 234 1 215 235 1 216 236 1 217 237 1 218 238 1 219 239 1 220 240 1 221 241 1 222 242 1
		 223 243 1 224 244 1 225 245 1 226 246 1 227 247 1 228 248 1 229 249 1 230 250 1 231 251 1
		 232 252 1 233 253 1 234 254 1 235 255 1 236 256 1 237 257 1 238 258 1 239 259 1 240 260 1
		 241 261 1 242 262 1 243 263 1 244 264 1 245 265 1 246 266 1 247 267 1 248 268 1 249 269 1
		 250 270 1 251 271 1 252 272 1 253 273 1 254 274 1 255 275 1 256 276 1 257 277 1 258 278 1
		 259 279 1 260 280 1 261 281 1 262 282 1 263 283 1 264 284 1 265 285 1 266 286 1 267 287 1
		 268 288 1 269 289 1 270 290 1 271 291 1 272 292 1 273 293 1 274 294 1 275 295 1 276 296 1
		 277 297 1 278 298 1 279 299 1 280 300 1 281 301 1 282 302 1 283 303 1;
	setAttr ".ed[664:779]" 284 304 1 285 305 1 286 306 1 287 307 1 288 308 1 289 309 1
		 290 310 1 291 311 1 292 312 1 293 313 1 294 314 1 295 315 1 296 316 1 297 317 1 298 318 1
		 299 319 1 300 320 1 301 321 1 302 322 1 303 323 1 304 324 1 305 325 1 306 326 1 307 327 1
		 308 328 1 309 329 1 310 330 1 311 331 1 312 332 1 313 333 1 314 334 1 315 335 1 316 336 1
		 317 337 1 318 338 1 319 339 1 320 340 1 321 341 1 322 342 1 323 343 1 324 344 1 325 345 1
		 326 346 1 327 347 1 328 348 1 329 349 1 330 350 1 331 351 1 332 352 1 333 353 1 334 354 1
		 335 355 1 336 356 1 337 357 1 338 358 1 339 359 1 340 360 1 341 361 1 342 362 1 343 363 1
		 344 364 1 345 365 1 346 366 1 347 367 1 348 368 1 349 369 1 350 370 1 351 371 1 352 372 1
		 353 373 1 354 374 1 355 375 1 356 376 1 357 377 1 358 378 1 359 379 1 380 0 1 380 1 1
		 380 2 1 380 3 1 380 4 1 380 5 1 380 6 1 380 7 1 380 8 1 380 9 1 380 10 1 380 11 1
		 380 12 1 380 13 1 380 14 1 380 15 1 380 16 1 380 17 1 380 18 1 380 19 1 360 381 1
		 361 381 1 362 381 1 363 381 1 364 381 1 365 381 1 366 381 1 367 381 1 368 381 1 369 381 1
		 370 381 1 371 381 1 372 381 1 373 381 1 374 381 1 375 381 1 376 381 1 377 381 1 378 381 1
		 379 381 1;
	setAttr -s 400 -ch 1560 ".fc[0:399]" -type "polyFaces" 
		f 4 0 381 -21 -381
		mu 0 4 0 1 22 21
		f 4 1 382 -22 -382
		mu 0 4 1 2 23 22
		f 4 2 383 -23 -383
		mu 0 4 2 3 24 23
		f 4 3 384 -24 -384
		mu 0 4 3 4 25 24
		f 4 4 385 -25 -385
		mu 0 4 4 5 26 25
		f 4 5 386 -26 -386
		mu 0 4 5 6 27 26
		f 4 6 387 -27 -387
		mu 0 4 6 7 28 27
		f 4 7 388 -28 -388
		mu 0 4 7 8 29 28
		f 4 8 389 -29 -389
		mu 0 4 8 9 30 29
		f 4 9 390 -30 -390
		mu 0 4 9 10 31 30
		f 4 10 391 -31 -391
		mu 0 4 10 11 32 31
		f 4 11 392 -32 -392
		mu 0 4 11 12 33 32
		f 4 12 393 -33 -393
		mu 0 4 12 13 34 33
		f 4 13 394 -34 -394
		mu 0 4 13 14 35 34
		f 4 14 395 -35 -395
		mu 0 4 14 15 36 35
		f 4 15 396 -36 -396
		mu 0 4 15 16 37 36
		f 4 16 397 -37 -397
		mu 0 4 16 17 38 37
		f 4 17 398 -38 -398
		mu 0 4 17 18 39 38
		f 4 18 399 -39 -399
		mu 0 4 18 19 40 39
		f 4 19 380 -40 -400
		mu 0 4 19 20 41 40
		f 4 20 401 -41 -401
		mu 0 4 21 22 43 42
		f 4 21 402 -42 -402
		mu 0 4 22 23 44 43
		f 4 22 403 -43 -403
		mu 0 4 23 24 45 44
		f 4 23 404 -44 -404
		mu 0 4 24 25 46 45
		f 4 24 405 -45 -405
		mu 0 4 25 26 47 46
		f 4 25 406 -46 -406
		mu 0 4 26 27 48 47
		f 4 26 407 -47 -407
		mu 0 4 27 28 49 48
		f 4 27 408 -48 -408
		mu 0 4 28 29 50 49
		f 4 28 409 -49 -409
		mu 0 4 29 30 51 50
		f 4 29 410 -50 -410
		mu 0 4 30 31 52 51
		f 4 30 411 -51 -411
		mu 0 4 31 32 53 52
		f 4 31 412 -52 -412
		mu 0 4 32 33 54 53
		f 4 32 413 -53 -413
		mu 0 4 33 34 55 54
		f 4 33 414 -54 -414
		mu 0 4 34 35 56 55
		f 4 34 415 -55 -415
		mu 0 4 35 36 57 56
		f 4 35 416 -56 -416
		mu 0 4 36 37 58 57
		f 4 36 417 -57 -417
		mu 0 4 37 38 59 58
		f 4 37 418 -58 -418
		mu 0 4 38 39 60 59
		f 4 38 419 -59 -419
		mu 0 4 39 40 61 60
		f 4 39 400 -60 -420
		mu 0 4 40 41 62 61
		f 4 40 421 -61 -421
		mu 0 4 42 43 64 63
		f 4 41 422 -62 -422
		mu 0 4 43 44 65 64
		f 4 42 423 -63 -423
		mu 0 4 44 45 66 65
		f 4 43 424 -64 -424
		mu 0 4 45 46 67 66
		f 4 44 425 -65 -425
		mu 0 4 46 47 68 67
		f 4 45 426 -66 -426
		mu 0 4 47 48 69 68
		f 4 46 427 -67 -427
		mu 0 4 48 49 70 69
		f 4 47 428 -68 -428
		mu 0 4 49 50 71 70
		f 4 48 429 -69 -429
		mu 0 4 50 51 72 71
		f 4 49 430 -70 -430
		mu 0 4 51 52 73 72
		f 4 50 431 -71 -431
		mu 0 4 52 53 74 73
		f 4 51 432 -72 -432
		mu 0 4 53 54 75 74
		f 4 52 433 -73 -433
		mu 0 4 54 55 76 75
		f 4 53 434 -74 -434
		mu 0 4 55 56 77 76
		f 4 54 435 -75 -435
		mu 0 4 56 57 78 77
		f 4 55 436 -76 -436
		mu 0 4 57 58 79 78
		f 4 56 437 -77 -437
		mu 0 4 58 59 80 79
		f 4 57 438 -78 -438
		mu 0 4 59 60 81 80
		f 4 58 439 -79 -439
		mu 0 4 60 61 82 81
		f 4 59 420 -80 -440
		mu 0 4 61 62 83 82
		f 4 60 441 -81 -441
		mu 0 4 63 64 85 84
		f 4 61 442 -82 -442
		mu 0 4 64 65 86 85
		f 4 62 443 -83 -443
		mu 0 4 65 66 87 86
		f 4 63 444 -84 -444
		mu 0 4 66 67 88 87
		f 4 64 445 -85 -445
		mu 0 4 67 68 89 88
		f 4 65 446 -86 -446
		mu 0 4 68 69 90 89
		f 4 66 447 -87 -447
		mu 0 4 69 70 91 90
		f 4 67 448 -88 -448
		mu 0 4 70 71 92 91
		f 4 68 449 -89 -449
		mu 0 4 71 72 93 92
		f 4 69 450 -90 -450
		mu 0 4 72 73 94 93
		f 4 70 451 -91 -451
		mu 0 4 73 74 95 94
		f 4 71 452 -92 -452
		mu 0 4 74 75 96 95
		f 4 72 453 -93 -453
		mu 0 4 75 76 97 96
		f 4 73 454 -94 -454
		mu 0 4 76 77 98 97
		f 4 74 455 -95 -455
		mu 0 4 77 78 99 98
		f 4 75 456 -96 -456
		mu 0 4 78 79 100 99
		f 4 76 457 -97 -457
		mu 0 4 79 80 101 100
		f 4 77 458 -98 -458
		mu 0 4 80 81 102 101
		f 4 78 459 -99 -459
		mu 0 4 81 82 103 102
		f 4 79 440 -100 -460
		mu 0 4 82 83 104 103
		f 4 80 461 -101 -461
		mu 0 4 84 85 106 105
		f 4 81 462 -102 -462
		mu 0 4 85 86 107 106
		f 4 82 463 -103 -463
		mu 0 4 86 87 108 107
		f 4 83 464 -104 -464
		mu 0 4 87 88 109 108
		f 4 84 465 -105 -465
		mu 0 4 88 89 110 109
		f 4 85 466 -106 -466
		mu 0 4 89 90 111 110
		f 4 86 467 -107 -467
		mu 0 4 90 91 112 111
		f 4 87 468 -108 -468
		mu 0 4 91 92 113 112
		f 4 88 469 -109 -469
		mu 0 4 92 93 114 113
		f 4 89 470 -110 -470
		mu 0 4 93 94 115 114
		f 4 90 471 -111 -471
		mu 0 4 94 95 116 115
		f 4 91 472 -112 -472
		mu 0 4 95 96 117 116
		f 4 92 473 -113 -473
		mu 0 4 96 97 118 117
		f 4 93 474 -114 -474
		mu 0 4 97 98 119 118
		f 4 94 475 -115 -475
		mu 0 4 98 99 120 119
		f 4 95 476 -116 -476
		mu 0 4 99 100 121 120
		f 4 96 477 -117 -477
		mu 0 4 100 101 122 121
		f 4 97 478 -118 -478
		mu 0 4 101 102 123 122
		f 4 98 479 -119 -479
		mu 0 4 102 103 124 123
		f 4 99 460 -120 -480
		mu 0 4 103 104 125 124
		f 4 100 481 -121 -481
		mu 0 4 105 106 127 126
		f 4 101 482 -122 -482
		mu 0 4 106 107 128 127
		f 4 102 483 -123 -483
		mu 0 4 107 108 129 128
		f 4 103 484 -124 -484
		mu 0 4 108 109 130 129
		f 4 104 485 -125 -485
		mu 0 4 109 110 131 130
		f 4 105 486 -126 -486
		mu 0 4 110 111 132 131
		f 4 106 487 -127 -487
		mu 0 4 111 112 133 132
		f 4 107 488 -128 -488
		mu 0 4 112 113 134 133
		f 4 108 489 -129 -489
		mu 0 4 113 114 135 134
		f 4 109 490 -130 -490
		mu 0 4 114 115 136 135
		f 4 110 491 -131 -491
		mu 0 4 115 116 137 136
		f 4 111 492 -132 -492
		mu 0 4 116 117 138 137
		f 4 112 493 -133 -493
		mu 0 4 117 118 139 138
		f 4 113 494 -134 -494
		mu 0 4 118 119 140 139
		f 4 114 495 -135 -495
		mu 0 4 119 120 141 140
		f 4 115 496 -136 -496
		mu 0 4 120 121 142 141
		f 4 116 497 -137 -497
		mu 0 4 121 122 143 142
		f 4 117 498 -138 -498
		mu 0 4 122 123 144 143
		f 4 118 499 -139 -499
		mu 0 4 123 124 145 144
		f 4 119 480 -140 -500
		mu 0 4 124 125 146 145
		f 4 120 501 -141 -501
		mu 0 4 126 127 148 147
		f 4 121 502 -142 -502
		mu 0 4 127 128 149 148
		f 4 122 503 -143 -503
		mu 0 4 128 129 150 149
		f 4 123 504 -144 -504
		mu 0 4 129 130 151 150
		f 4 124 505 -145 -505
		mu 0 4 130 131 152 151
		f 4 125 506 -146 -506
		mu 0 4 131 132 153 152
		f 4 126 507 -147 -507
		mu 0 4 132 133 154 153
		f 4 127 508 -148 -508
		mu 0 4 133 134 155 154
		f 4 128 509 -149 -509
		mu 0 4 134 135 156 155
		f 4 129 510 -150 -510
		mu 0 4 135 136 157 156
		f 4 130 511 -151 -511
		mu 0 4 136 137 158 157
		f 4 131 512 -152 -512
		mu 0 4 137 138 159 158
		f 4 132 513 -153 -513
		mu 0 4 138 139 160 159
		f 4 133 514 -154 -514
		mu 0 4 139 140 161 160
		f 4 134 515 -155 -515
		mu 0 4 140 141 162 161
		f 4 135 516 -156 -516
		mu 0 4 141 142 163 162
		f 4 136 517 -157 -517
		mu 0 4 142 143 164 163
		f 4 137 518 -158 -518
		mu 0 4 143 144 165 164
		f 4 138 519 -159 -519
		mu 0 4 144 145 166 165
		f 4 139 500 -160 -520
		mu 0 4 145 146 167 166
		f 4 140 521 -161 -521
		mu 0 4 147 148 169 168
		f 4 141 522 -162 -522
		mu 0 4 148 149 170 169
		f 4 142 523 -163 -523
		mu 0 4 149 150 171 170
		f 4 143 524 -164 -524
		mu 0 4 150 151 172 171
		f 4 144 525 -165 -525
		mu 0 4 151 152 173 172
		f 4 145 526 -166 -526
		mu 0 4 152 153 174 173
		f 4 146 527 -167 -527
		mu 0 4 153 154 175 174
		f 4 147 528 -168 -528
		mu 0 4 154 155 176 175
		f 4 148 529 -169 -529
		mu 0 4 155 156 177 176
		f 4 149 530 -170 -530
		mu 0 4 156 157 178 177
		f 4 150 531 -171 -531
		mu 0 4 157 158 179 178
		f 4 151 532 -172 -532
		mu 0 4 158 159 180 179
		f 4 152 533 -173 -533
		mu 0 4 159 160 181 180
		f 4 153 534 -174 -534
		mu 0 4 160 161 182 181
		f 4 154 535 -175 -535
		mu 0 4 161 162 183 182
		f 4 155 536 -176 -536
		mu 0 4 162 163 184 183
		f 4 156 537 -177 -537
		mu 0 4 163 164 185 184
		f 4 157 538 -178 -538
		mu 0 4 164 165 186 185
		f 4 158 539 -179 -539
		mu 0 4 165 166 187 186
		f 4 159 520 -180 -540
		mu 0 4 166 167 188 187
		f 4 160 541 -181 -541
		mu 0 4 168 169 190 189
		f 4 161 542 -182 -542
		mu 0 4 169 170 191 190
		f 4 162 543 -183 -543
		mu 0 4 170 171 192 191
		f 4 163 544 -184 -544
		mu 0 4 171 172 193 192
		f 4 164 545 -185 -545
		mu 0 4 172 173 194 193
		f 4 165 546 -186 -546
		mu 0 4 173 174 195 194
		f 4 166 547 -187 -547
		mu 0 4 174 175 196 195
		f 4 167 548 -188 -548
		mu 0 4 175 176 197 196
		f 4 168 549 -189 -549
		mu 0 4 176 177 198 197
		f 4 169 550 -190 -550
		mu 0 4 177 178 199 198
		f 4 170 551 -191 -551
		mu 0 4 178 179 200 199
		f 4 171 552 -192 -552
		mu 0 4 179 180 201 200
		f 4 172 553 -193 -553
		mu 0 4 180 181 202 201
		f 4 173 554 -194 -554
		mu 0 4 181 182 203 202
		f 4 174 555 -195 -555
		mu 0 4 182 183 204 203
		f 4 175 556 -196 -556
		mu 0 4 183 184 205 204
		f 4 176 557 -197 -557
		mu 0 4 184 185 206 205
		f 4 177 558 -198 -558
		mu 0 4 185 186 207 206
		f 4 178 559 -199 -559
		mu 0 4 186 187 208 207
		f 4 179 540 -200 -560
		mu 0 4 187 188 209 208
		f 4 180 561 -201 -561
		mu 0 4 189 190 211 210
		f 4 181 562 -202 -562
		mu 0 4 190 191 212 211
		f 4 182 563 -203 -563
		mu 0 4 191 192 213 212
		f 4 183 564 -204 -564
		mu 0 4 192 193 214 213
		f 4 184 565 -205 -565
		mu 0 4 193 194 215 214
		f 4 185 566 -206 -566
		mu 0 4 194 195 216 215
		f 4 186 567 -207 -567
		mu 0 4 195 196 217 216
		f 4 187 568 -208 -568
		mu 0 4 196 197 218 217
		f 4 188 569 -209 -569
		mu 0 4 197 198 219 218
		f 4 189 570 -210 -570
		mu 0 4 198 199 220 219
		f 4 190 571 -211 -571
		mu 0 4 199 200 221 220
		f 4 191 572 -212 -572
		mu 0 4 200 201 222 221
		f 4 192 573 -213 -573
		mu 0 4 201 202 223 222
		f 4 193 574 -214 -574
		mu 0 4 202 203 224 223
		f 4 194 575 -215 -575
		mu 0 4 203 204 225 224
		f 4 195 576 -216 -576
		mu 0 4 204 205 226 225
		f 4 196 577 -217 -577
		mu 0 4 205 206 227 226
		f 4 197 578 -218 -578
		mu 0 4 206 207 228 227
		f 4 198 579 -219 -579
		mu 0 4 207 208 229 228
		f 4 199 560 -220 -580
		mu 0 4 208 209 230 229
		f 4 200 581 -221 -581
		mu 0 4 210 211 232 231
		f 4 201 582 -222 -582
		mu 0 4 211 212 233 232
		f 4 202 583 -223 -583
		mu 0 4 212 213 234 233
		f 4 203 584 -224 -584
		mu 0 4 213 214 235 234
		f 4 204 585 -225 -585
		mu 0 4 214 215 236 235
		f 4 205 586 -226 -586
		mu 0 4 215 216 237 236
		f 4 206 587 -227 -587
		mu 0 4 216 217 238 237
		f 4 207 588 -228 -588
		mu 0 4 217 218 239 238
		f 4 208 589 -229 -589
		mu 0 4 218 219 240 239
		f 4 209 590 -230 -590
		mu 0 4 219 220 241 240
		f 4 210 591 -231 -591
		mu 0 4 220 221 242 241
		f 4 211 592 -232 -592
		mu 0 4 221 222 243 242
		f 4 212 593 -233 -593
		mu 0 4 222 223 244 243
		f 4 213 594 -234 -594
		mu 0 4 223 224 245 244
		f 4 214 595 -235 -595
		mu 0 4 224 225 246 245
		f 4 215 596 -236 -596
		mu 0 4 225 226 247 246
		f 4 216 597 -237 -597
		mu 0 4 226 227 248 247
		f 4 217 598 -238 -598
		mu 0 4 227 228 249 248
		f 4 218 599 -239 -599
		mu 0 4 228 229 250 249
		f 4 219 580 -240 -600
		mu 0 4 229 230 251 250
		f 4 220 601 -241 -601
		mu 0 4 231 232 253 252
		f 4 221 602 -242 -602
		mu 0 4 232 233 254 253
		f 4 222 603 -243 -603
		mu 0 4 233 234 255 254
		f 4 223 604 -244 -604
		mu 0 4 234 235 256 255
		f 4 224 605 -245 -605
		mu 0 4 235 236 257 256
		f 4 225 606 -246 -606
		mu 0 4 236 237 258 257
		f 4 226 607 -247 -607
		mu 0 4 237 238 259 258
		f 4 227 608 -248 -608
		mu 0 4 238 239 260 259
		f 4 228 609 -249 -609
		mu 0 4 239 240 261 260
		f 4 229 610 -250 -610
		mu 0 4 240 241 262 261
		f 4 230 611 -251 -611
		mu 0 4 241 242 263 262
		f 4 231 612 -252 -612
		mu 0 4 242 243 264 263
		f 4 232 613 -253 -613
		mu 0 4 243 244 265 264
		f 4 233 614 -254 -614
		mu 0 4 244 245 266 265
		f 4 234 615 -255 -615
		mu 0 4 245 246 267 266
		f 4 235 616 -256 -616
		mu 0 4 246 247 268 267
		f 4 236 617 -257 -617
		mu 0 4 247 248 269 268
		f 4 237 618 -258 -618
		mu 0 4 248 249 270 269
		f 4 238 619 -259 -619
		mu 0 4 249 250 271 270
		f 4 239 600 -260 -620
		mu 0 4 250 251 272 271
		f 4 240 621 -261 -621
		mu 0 4 252 253 274 273
		f 4 241 622 -262 -622
		mu 0 4 253 254 275 274
		f 4 242 623 -263 -623
		mu 0 4 254 255 276 275
		f 4 243 624 -264 -624
		mu 0 4 255 256 277 276
		f 4 244 625 -265 -625
		mu 0 4 256 257 278 277
		f 4 245 626 -266 -626
		mu 0 4 257 258 279 278
		f 4 246 627 -267 -627
		mu 0 4 258 259 280 279
		f 4 247 628 -268 -628
		mu 0 4 259 260 281 280
		f 4 248 629 -269 -629
		mu 0 4 260 261 282 281
		f 4 249 630 -270 -630
		mu 0 4 261 262 283 282
		f 4 250 631 -271 -631
		mu 0 4 262 263 284 283
		f 4 251 632 -272 -632
		mu 0 4 263 264 285 284
		f 4 252 633 -273 -633
		mu 0 4 264 265 286 285
		f 4 253 634 -274 -634
		mu 0 4 265 266 287 286
		f 4 254 635 -275 -635
		mu 0 4 266 267 288 287
		f 4 255 636 -276 -636
		mu 0 4 267 268 289 288
		f 4 256 637 -277 -637
		mu 0 4 268 269 290 289
		f 4 257 638 -278 -638
		mu 0 4 269 270 291 290
		f 4 258 639 -279 -639
		mu 0 4 270 271 292 291
		f 4 259 620 -280 -640
		mu 0 4 271 272 293 292
		f 4 260 641 -281 -641
		mu 0 4 273 274 295 294
		f 4 261 642 -282 -642
		mu 0 4 274 275 296 295
		f 4 262 643 -283 -643
		mu 0 4 275 276 297 296
		f 4 263 644 -284 -644
		mu 0 4 276 277 298 297
		f 4 264 645 -285 -645
		mu 0 4 277 278 299 298
		f 4 265 646 -286 -646
		mu 0 4 278 279 300 299
		f 4 266 647 -287 -647
		mu 0 4 279 280 301 300
		f 4 267 648 -288 -648
		mu 0 4 280 281 302 301
		f 4 268 649 -289 -649
		mu 0 4 281 282 303 302
		f 4 269 650 -290 -650
		mu 0 4 282 283 304 303
		f 4 270 651 -291 -651
		mu 0 4 283 284 305 304
		f 4 271 652 -292 -652
		mu 0 4 284 285 306 305
		f 4 272 653 -293 -653
		mu 0 4 285 286 307 306
		f 4 273 654 -294 -654
		mu 0 4 286 287 308 307
		f 4 274 655 -295 -655
		mu 0 4 287 288 309 308
		f 4 275 656 -296 -656
		mu 0 4 288 289 310 309
		f 4 276 657 -297 -657
		mu 0 4 289 290 311 310
		f 4 277 658 -298 -658
		mu 0 4 290 291 312 311
		f 4 278 659 -299 -659
		mu 0 4 291 292 313 312
		f 4 279 640 -300 -660
		mu 0 4 292 293 314 313
		f 4 280 661 -301 -661
		mu 0 4 294 295 316 315
		f 4 281 662 -302 -662
		mu 0 4 295 296 317 316
		f 4 282 663 -303 -663
		mu 0 4 296 297 318 317
		f 4 283 664 -304 -664
		mu 0 4 297 298 319 318
		f 4 284 665 -305 -665
		mu 0 4 298 299 320 319
		f 4 285 666 -306 -666
		mu 0 4 299 300 321 320
		f 4 286 667 -307 -667
		mu 0 4 300 301 322 321
		f 4 287 668 -308 -668
		mu 0 4 301 302 323 322
		f 4 288 669 -309 -669
		mu 0 4 302 303 324 323
		f 4 289 670 -310 -670
		mu 0 4 303 304 325 324
		f 4 290 671 -311 -671
		mu 0 4 304 305 326 325
		f 4 291 672 -312 -672
		mu 0 4 305 306 327 326
		f 4 292 673 -313 -673
		mu 0 4 306 307 328 327
		f 4 293 674 -314 -674
		mu 0 4 307 308 329 328
		f 4 294 675 -315 -675
		mu 0 4 308 309 330 329
		f 4 295 676 -316 -676
		mu 0 4 309 310 331 330
		f 4 296 677 -317 -677
		mu 0 4 310 311 332 331
		f 4 297 678 -318 -678
		mu 0 4 311 312 333 332
		f 4 298 679 -319 -679
		mu 0 4 312 313 334 333
		f 4 299 660 -320 -680
		mu 0 4 313 314 335 334
		f 4 300 681 -321 -681
		mu 0 4 315 316 337 336
		f 4 301 682 -322 -682
		mu 0 4 316 317 338 337
		f 4 302 683 -323 -683
		mu 0 4 317 318 339 338
		f 4 303 684 -324 -684
		mu 0 4 318 319 340 339
		f 4 304 685 -325 -685
		mu 0 4 319 320 341 340
		f 4 305 686 -326 -686
		mu 0 4 320 321 342 341
		f 4 306 687 -327 -687
		mu 0 4 321 322 343 342
		f 4 307 688 -328 -688
		mu 0 4 322 323 344 343
		f 4 308 689 -329 -689
		mu 0 4 323 324 345 344
		f 4 309 690 -330 -690
		mu 0 4 324 325 346 345
		f 4 310 691 -331 -691
		mu 0 4 325 326 347 346
		f 4 311 692 -332 -692
		mu 0 4 326 327 348 347
		f 4 312 693 -333 -693
		mu 0 4 327 328 349 348
		f 4 313 694 -334 -694
		mu 0 4 328 329 350 349
		f 4 314 695 -335 -695
		mu 0 4 329 330 351 350
		f 4 315 696 -336 -696
		mu 0 4 330 331 352 351
		f 4 316 697 -337 -697
		mu 0 4 331 332 353 352
		f 4 317 698 -338 -698
		mu 0 4 332 333 354 353
		f 4 318 699 -339 -699
		mu 0 4 333 334 355 354
		f 4 319 680 -340 -700
		mu 0 4 334 335 356 355
		f 4 320 701 -341 -701
		mu 0 4 336 337 358 357
		f 4 321 702 -342 -702
		mu 0 4 337 338 359 358
		f 4 322 703 -343 -703
		mu 0 4 338 339 360 359
		f 4 323 704 -344 -704
		mu 0 4 339 340 361 360
		f 4 324 705 -345 -705
		mu 0 4 340 341 362 361
		f 4 325 706 -346 -706
		mu 0 4 341 342 363 362
		f 4 326 707 -347 -707
		mu 0 4 342 343 364 363
		f 4 327 708 -348 -708
		mu 0 4 343 344 365 364
		f 4 328 709 -349 -709
		mu 0 4 344 345 366 365
		f 4 329 710 -350 -710
		mu 0 4 345 346 367 366
		f 4 330 711 -351 -711
		mu 0 4 346 347 368 367
		f 4 331 712 -352 -712
		mu 0 4 347 348 369 368
		f 4 332 713 -353 -713
		mu 0 4 348 349 370 369
		f 4 333 714 -354 -714
		mu 0 4 349 350 371 370
		f 4 334 715 -355 -715
		mu 0 4 350 351 372 371
		f 4 335 716 -356 -716
		mu 0 4 351 352 373 372
		f 4 336 717 -357 -717
		mu 0 4 352 353 374 373
		f 4 337 718 -358 -718
		mu 0 4 353 354 375 374
		f 4 338 719 -359 -719
		mu 0 4 354 355 376 375
		f 4 339 700 -360 -720
		mu 0 4 355 356 377 376
		f 4 340 721 -361 -721
		mu 0 4 357 358 379 378
		f 4 341 722 -362 -722
		mu 0 4 358 359 380 379
		f 4 342 723 -363 -723
		mu 0 4 359 360 381 380
		f 4 343 724 -364 -724
		mu 0 4 360 361 382 381
		f 4 344 725 -365 -725
		mu 0 4 361 362 383 382
		f 4 345 726 -366 -726
		mu 0 4 362 363 384 383
		f 4 346 727 -367 -727
		mu 0 4 363 364 385 384
		f 4 347 728 -368 -728
		mu 0 4 364 365 386 385
		f 4 348 729 -369 -729
		mu 0 4 365 366 387 386
		f 4 349 730 -370 -730
		mu 0 4 366 367 388 387
		f 4 350 731 -371 -731
		mu 0 4 367 368 389 388
		f 4 351 732 -372 -732
		mu 0 4 368 369 390 389
		f 4 352 733 -373 -733
		mu 0 4 369 370 391 390
		f 4 353 734 -374 -734
		mu 0 4 370 371 392 391
		f 4 354 735 -375 -735
		mu 0 4 371 372 393 392
		f 4 355 736 -376 -736
		mu 0 4 372 373 394 393
		f 4 356 737 -377 -737
		mu 0 4 373 374 395 394
		f 4 357 738 -378 -738
		mu 0 4 374 375 396 395
		f 4 358 739 -379 -739
		mu 0 4 375 376 397 396
		f 4 359 720 -380 -740
		mu 0 4 376 377 398 397
		f 3 -1 -741 741
		mu 0 3 1 0 399
		f 3 -2 -742 742
		mu 0 3 2 1 400
		f 3 -3 -743 743
		mu 0 3 3 2 401
		f 3 -4 -744 744
		mu 0 3 4 3 402
		f 3 -5 -745 745
		mu 0 3 5 4 403
		f 3 -6 -746 746
		mu 0 3 6 5 404
		f 3 -7 -747 747
		mu 0 3 7 6 405
		f 3 -8 -748 748
		mu 0 3 8 7 406
		f 3 -9 -749 749
		mu 0 3 9 8 407
		f 3 -10 -750 750
		mu 0 3 10 9 408
		f 3 -11 -751 751
		mu 0 3 11 10 409
		f 3 -12 -752 752
		mu 0 3 12 11 410
		f 3 -13 -753 753
		mu 0 3 13 12 411
		f 3 -14 -754 754
		mu 0 3 14 13 412
		f 3 -15 -755 755
		mu 0 3 15 14 413
		f 3 -16 -756 756
		mu 0 3 16 15 414
		f 3 -17 -757 757
		mu 0 3 17 16 415
		f 3 -18 -758 758
		mu 0 3 18 17 416
		f 3 -19 -759 759
		mu 0 3 19 18 417
		f 3 -20 -760 740
		mu 0 3 20 19 418
		f 3 360 761 -761
		mu 0 3 378 379 419
		f 3 361 762 -762
		mu 0 3 379 380 420
		f 3 362 763 -763
		mu 0 3 380 381 421
		f 3 363 764 -764
		mu 0 3 381 382 422
		f 3 364 765 -765
		mu 0 3 382 383 423
		f 3 365 766 -766
		mu 0 3 383 384 424
		f 3 366 767 -767
		mu 0 3 384 385 425
		f 3 367 768 -768
		mu 0 3 385 386 426
		f 3 368 769 -769
		mu 0 3 386 387 427
		f 3 369 770 -770
		mu 0 3 387 388 428
		f 3 370 771 -771
		mu 0 3 388 389 429
		f 3 371 772 -772
		mu 0 3 389 390 430
		f 3 372 773 -773
		mu 0 3 390 391 431
		f 3 373 774 -774
		mu 0 3 391 392 432
		f 3 374 775 -775
		mu 0 3 392 393 433
		f 3 375 776 -776
		mu 0 3 393 394 434
		f 3 376 777 -777
		mu 0 3 394 395 435
		f 3 377 778 -778
		mu 0 3 395 396 436
		f 3 378 779 -779
		mu 0 3 396 397 437
		f 3 379 760 -780
		mu 0 3 397 398 438;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode transform -n "buddy_eye_r" -p "geometry_GRP";
	rename -uid "9BE9D776-4CA4-B964-DBF6-28B0ED6C1E53";
	setAttr -l on ".tx";
	setAttr -l on ".ty";
	setAttr -l on ".tz";
	setAttr ".r" -type "double3" 0 -14.999999999999998 0 ;
	setAttr -l on ".rx";
	setAttr -l on ".ry";
	setAttr -l on ".rz";
	setAttr -l on ".sx";
	setAttr -l on ".sy";
	setAttr -l on ".sz";
	setAttr ".rp" -type "double3" -4.9018420950466242 38.174055827991289 11.108802664824765 ;
	setAttr ".rpt" -type "double3" -2.7081434788921075 0 -1.6472133620050071 ;
	setAttr ".sp" -type "double3" -4.9018420950466242 38.174055827991289 11.108802664824765 ;
	setAttr ".it" no;
createNode mesh -n "buddy_eye_rShape" -p "buddy_eye_r";
	rename -uid "09ACB1E7-4E58-9DB4-D957-8994AC22A191";
	setAttr -k off ".v";
	setAttr -s 4 ".iog[0].og";
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".pv" -type "double2" 0.095608620739999997 0.098410180139999998 ;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr ".ndt" 0;
	setAttr ".dr" 3;
	setAttr ".dsm" 2;
	setAttr ".vcs" 2;
createNode mesh -n "buddy_eye_rShapeOrig" -p "buddy_eye_r";
	rename -uid "B0965556-4817-D127-D236-6F9ED65BAE93";
	setAttr -k off ".v";
	setAttr ".io" yes;
	setAttr ".vir" yes;
	setAttr ".vif" yes;
	setAttr ".uvst[0].uvsn" -type "string" "map1";
	setAttr -s 439 ".uvst[0].uvsp";
	setAttr ".uvst[0].uvsp[0:249]" -type "float2" 0.0042232382 0.011800933 0.011800926
		 0.011800933 0.019378584 0.011800933 0.026956271 0.011800933 0.034533925 0.011800933
		 0.042111613 0.011800933 0.049689267 0.011800933 0.057266954 0.011800933 0.064844646
		 0.011800933 0.072422303 0.011800933 0.079999991 0.011800933 0.087577678 0.011800933
		 0.095155366 0.011800933 0.10273299 0.011800933 0.11031068 0.011800933 0.11788837
		 0.011800933 0.12546608 0.011800933 0.13304375 0.011800933 0.14062144 0.011800933
		 0.14819905 0.011800933 0.15577677 0.011800933 0.0042232382 0.019378591 0.011800926
		 0.019378591 0.019378584 0.019378591 0.026956271 0.019378591 0.034533925 0.019378591
		 0.042111613 0.019378591 0.049689267 0.019378591 0.057266954 0.019378591 0.064844646
		 0.019378591 0.072422303 0.019378591 0.079999991 0.019378591 0.087577678 0.019378591
		 0.095155366 0.019378591 0.10273299 0.019378591 0.11031068 0.019378591 0.11788837
		 0.019378591 0.12546608 0.019378591 0.13304375 0.019378591 0.14062144 0.019378591
		 0.14819905 0.019378591 0.15577677 0.019378591 0.0042232382 0.026956286 0.011800926
		 0.026956286 0.019378584 0.026956286 0.026956271 0.026956286 0.034533925 0.026956286
		 0.042111613 0.026956286 0.049689267 0.026956286 0.057266954 0.026956286 0.064844646
		 0.026956286 0.072422303 0.026956286 0.079999991 0.026956286 0.087577678 0.026956286
		 0.095155366 0.026956286 0.10273299 0.026956286 0.11031068 0.026956286 0.11788837
		 0.026956286 0.12546608 0.026956286 0.13304375 0.026956286 0.14062144 0.026956286
		 0.14819905 0.026956286 0.15577677 0.026956286 0.0042232382 0.03453394 0.011800926
		 0.03453394 0.019378584 0.03453394 0.026956271 0.03453394 0.034533925 0.03453394 0.042111613
		 0.03453394 0.049689267 0.03453394 0.057266954 0.03453394 0.064844646 0.03453394 0.072422303
		 0.03453394 0.079999991 0.03453394 0.087577678 0.03453394 0.095155366 0.03453394 0.10273299
		 0.03453394 0.11031068 0.03453394 0.11788837 0.03453394 0.12546608 0.03453394 0.13304375
		 0.03453394 0.14062144 0.03453394 0.14819905 0.03453394 0.15577677 0.03453394 0.0042232382
		 0.042111628 0.011800926 0.042111628 0.019378584 0.042111628 0.026956271 0.042111628
		 0.034533925 0.042111628 0.042111613 0.042111628 0.049689267 0.042111628 0.057266954
		 0.042111628 0.064844646 0.042111628 0.072422303 0.042111628 0.079999991 0.042111628
		 0.087577678 0.042111628 0.095155366 0.042111628 0.10273299 0.042111628 0.11031068
		 0.042111628 0.11788837 0.042111628 0.12546608 0.042111628 0.13304375 0.042111628
		 0.14062144 0.042111628 0.14819905 0.042111628 0.15577677 0.042111628 0.0042232382
		 0.049689312 0.011800926 0.049689312 0.019378584 0.049689312 0.026956271 0.049689312
		 0.034533925 0.049689312 0.042111613 0.049689312 0.049689267 0.049689312 0.057266954
		 0.049689312 0.064844646 0.049689312 0.072422303 0.049689312 0.079999991 0.049689312
		 0.087577678 0.049689312 0.095155366 0.049689312 0.10273299 0.049689312 0.11031068
		 0.049689312 0.11788837 0.049689312 0.12546608 0.049689312 0.13304375 0.049689312
		 0.14062144 0.049689312 0.14819905 0.049689312 0.15577677 0.049689312 0.0042232382
		 0.057266966 0.011800926 0.057266966 0.019378584 0.057266966 0.026956271 0.057266966
		 0.034533925 0.057266966 0.042111613 0.057266966 0.049689267 0.057266966 0.057266954
		 0.057266966 0.064844646 0.057266966 0.072422303 0.057266966 0.079999991 0.057266966
		 0.087577678 0.057266966 0.095155366 0.057266966 0.10273299 0.057266966 0.11031068
		 0.057266966 0.11788837 0.057266966 0.12546608 0.057266966 0.13304375 0.057266966
		 0.14062144 0.057266966 0.14819905 0.057266966 0.15577677 0.057266966 0.0042232382
		 0.064844653 0.011800926 0.064844653 0.019378584 0.064844653 0.026956271 0.064844653
		 0.034533925 0.064844653 0.042111613 0.064844653 0.049689267 0.064844653 0.057266954
		 0.064844653 0.064844646 0.064844653 0.072422303 0.064844653 0.079999991 0.064844653
		 0.087577678 0.064844653 0.095155366 0.064844653 0.10273299 0.064844653 0.11031068
		 0.064844653 0.11788837 0.064844653 0.12546608 0.064844653 0.13304375 0.064844653
		 0.14062144 0.064844653 0.14819905 0.064844653 0.15577677 0.064844653 0.0042232382
		 0.072422341 0.011800926 0.072422341 0.019378584 0.072422341 0.026956271 0.072422341
		 0.034533925 0.072422341 0.042111613 0.072422341 0.049689267 0.072422341 0.057266954
		 0.072422341 0.064844646 0.072422341 0.072422303 0.072422341 0.079999991 0.072422341
		 0.087577678 0.072422341 0.095155366 0.072422341 0.10273299 0.072422341 0.11031068
		 0.072422341 0.11788837 0.072422341 0.12546608 0.072422341 0.13304375 0.072422341
		 0.14062144 0.072422341 0.14819905 0.072422341 0.15577677 0.072422341 0.0042232382
		 0.079999998 0.011800926 0.079999998 0.019378584 0.079999998 0.026956271 0.079999998
		 0.034533925 0.079999998 0.042111613 0.079999998 0.049689267 0.079999998 0.057266954
		 0.079999998 0.064844646 0.079999998 0.072422303 0.079999998 0.079999991 0.079999998
		 0.087577678 0.079999998 0.095155366 0.079999998 0.10273299 0.079999998 0.11031068
		 0.079999998 0.11788837 0.079999998 0.12546608 0.079999998 0.13304375 0.079999998
		 0.14062144 0.079999998 0.14819905 0.079999998 0.15577677 0.079999998 0.0042232382
		 0.087577686 0.011800926 0.087577686 0.019378584 0.087577686 0.026956271 0.087577686
		 0.034533925 0.087577686 0.042111613 0.087577686 0.049689267 0.087577686 0.057266954
		 0.087577686 0.064844646 0.087577686 0.072422303 0.087577686 0.079999991 0.087577686
		 0.087577678 0.087577686 0.095155366 0.087577686 0.10273299 0.087577686 0.11031068
		 0.087577686 0.11788837 0.087577686 0.12546608 0.087577686 0.13304375 0.087577686
		 0.14062144 0.087577686 0.14819905 0.087577686 0.15577677 0.087577686 0.0042232382
		 0.095155373 0.011800926 0.095155373 0.019378584 0.095155373 0.026956271 0.095155373
		 0.034533925 0.095155373 0.042111613 0.095155373 0.049689267 0.095155373 0.057266954
		 0.095155373 0.064844646 0.095155373 0.072422303 0.095155373 0.079999991 0.095155373
		 0.087577678 0.095155373 0.095155366 0.095155373 0.10273299 0.095155373 0.11031068
		 0.095155373 0.11788837 0.095155373 0.12546608 0.095155373 0.13304375 0.095155373
		 0.14062144 0.095155373;
	setAttr ".uvst[0].uvsp[250:438]" 0.14819905 0.095155373 0.15577677 0.095155373
		 0.0042232382 0.10273306 0.011800926 0.10273306 0.019378584 0.10273306 0.026956271
		 0.10273306 0.034533925 0.10273306 0.042111613 0.10273306 0.049689267 0.10273306 0.057266954
		 0.10273306 0.064844646 0.10273306 0.072422303 0.10273306 0.079999991 0.10273306 0.087577678
		 0.10273306 0.095155366 0.10273306 0.10273299 0.10273306 0.11031068 0.10273306 0.11788837
		 0.10273306 0.12546608 0.10273306 0.13304375 0.10273306 0.14062144 0.10273306 0.14819905
		 0.10273306 0.15577677 0.10273306 0.0042232382 0.11031069 0.011800926 0.11031069 0.019378584
		 0.11031069 0.026956271 0.11031069 0.034533925 0.11031069 0.042111613 0.11031069 0.049689267
		 0.11031069 0.057266954 0.11031069 0.064844646 0.11031069 0.072422303 0.11031069 0.079999991
		 0.11031069 0.087577678 0.11031069 0.095155366 0.11031069 0.10273299 0.11031069 0.11031068
		 0.11031069 0.11788837 0.11031069 0.12546608 0.11031069 0.13304375 0.11031069 0.14062144
		 0.11031069 0.14819905 0.11031069 0.15577677 0.11031069 0.0042232382 0.11788838 0.011800926
		 0.11788838 0.019378584 0.11788838 0.026956271 0.11788838 0.034533925 0.11788838 0.042111613
		 0.11788838 0.049689267 0.11788838 0.057266954 0.11788838 0.064844646 0.11788838 0.072422303
		 0.11788838 0.079999991 0.11788838 0.087577678 0.11788838 0.095155366 0.11788838 0.10273299
		 0.11788838 0.11031068 0.11788838 0.11788837 0.11788838 0.12546608 0.11788838 0.13304375
		 0.11788838 0.14062144 0.11788838 0.14819905 0.11788838 0.15577677 0.11788838 0.0042232382
		 0.12546606 0.011800926 0.12546606 0.019378584 0.12546606 0.026956271 0.12546606 0.034533925
		 0.12546606 0.042111613 0.12546606 0.049689267 0.12546606 0.057266954 0.12546606 0.064844646
		 0.12546606 0.072422303 0.12546606 0.079999991 0.12546606 0.087577678 0.12546606 0.095155366
		 0.12546606 0.10273299 0.12546606 0.11031068 0.12546606 0.11788837 0.12546606 0.12546608
		 0.12546606 0.13304375 0.12546606 0.14062144 0.12546606 0.14819905 0.12546606 0.15577677
		 0.12546606 0.0042232382 0.13304375 0.011800926 0.13304375 0.019378584 0.13304375
		 0.026956271 0.13304375 0.034533925 0.13304375 0.042111613 0.13304375 0.049689267
		 0.13304375 0.057266954 0.13304375 0.064844646 0.13304375 0.072422303 0.13304375 0.079999991
		 0.13304375 0.087577678 0.13304375 0.095155366 0.13304375 0.10273299 0.13304375 0.11031068
		 0.13304375 0.11788837 0.13304375 0.12546608 0.13304375 0.13304375 0.13304375 0.14062144
		 0.13304375 0.14819905 0.13304375 0.15577677 0.13304375 0.0042232382 0.14062144 0.011800926
		 0.14062144 0.019378584 0.14062144 0.026956271 0.14062144 0.034533925 0.14062144 0.042111613
		 0.14062144 0.049689267 0.14062144 0.057266954 0.14062144 0.064844646 0.14062144 0.072422303
		 0.14062144 0.079999991 0.14062144 0.087577678 0.14062144 0.095155366 0.14062144 0.10273299
		 0.14062144 0.11031068 0.14062144 0.11788837 0.14062144 0.12546608 0.14062144 0.13304375
		 0.14062144 0.14062144 0.14062144 0.14819905 0.14062144 0.15577677 0.14062144 0.0042232382
		 0.14819905 0.011800926 0.14819905 0.019378584 0.14819905 0.026956271 0.14819905 0.034533925
		 0.14819905 0.042111613 0.14819905 0.049689267 0.14819905 0.057266954 0.14819905 0.064844646
		 0.14819905 0.072422303 0.14819905 0.079999991 0.14819905 0.087577678 0.14819905 0.095155366
		 0.14819905 0.10273299 0.14819905 0.11031068 0.14819905 0.11788837 0.14819905 0.12546608
		 0.14819905 0.13304375 0.14819905 0.14062144 0.14819905 0.14819905 0.14819905 0.15577677
		 0.14819905 0.0080120824 0.0042232461 0.01558971 0.0042232461 0.023167428 0.0042232461
		 0.030745082 0.0042232461 0.038322769 0.0042232461 0.045900453 0.0042232461 0.053478111
		 0.0042232461 0.061055802 0.0042232461 0.06863346 0.0042232461 0.076211147 0.0042232461
		 0.083788835 0.0042232461 0.091366462 0.0042232461 0.09894415 0.0042232461 0.10652184
		 0.0042232461 0.11409952 0.0042232461 0.12167721 0.0042232461 0.12925485 0.0042232461
		 0.13683254 0.0042232461 0.14441022 0.0042232461 0.15198791 0.0042232461 0.0080120824
		 0.15577675 0.01558971 0.15577675 0.023167428 0.15577675 0.030745082 0.15577675 0.038322769
		 0.15577675 0.045900453 0.15577675 0.053478111 0.15577675 0.061055802 0.15577675 0.06863346
		 0.15577675 0.076211147 0.15577675 0.083788835 0.15577675 0.091366462 0.15577675 0.09894415
		 0.15577675 0.10652184 0.15577675 0.11409952 0.15577675 0.12167721 0.15577675 0.12925485
		 0.15577675 0.13683254 0.15577675 0.14441022 0.15577675 0.15198791 0.15577675;
	setAttr ".cuvs" -type "string" "map1";
	setAttr ".dcc" -type "string" "Ambient+Diffuse";
	setAttr ".covm[0]"  0 1 1;
	setAttr ".cdvm[0]"  0 1 1;
	setAttr -s 382 ".pt";
	setAttr ".pt[0:165]" -type "float3"  -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803;
	setAttr ".pt[166:331]" -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803;
	setAttr ".pt[332:381]" -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 
		38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 
		-4.9018421 38.174057 11.108803 -4.9018421 38.174057 11.108803 -4.9018421 38.174057 
		11.108803;
	setAttr -s 382 ".vt";
	setAttr ".vt[0:165]"  1.33869112 0.4349671 -8.88712406 1.13875866 0.82735658 -8.88712406
		 0.82735658 1.13875866 -8.88712406 0.43496704 1.338691 -8.88712406 -8.8817842e-16 1.40758288 -8.88712406
		 -0.43496704 1.33869088 -8.88712406 -0.8273564 1.13875842 -8.88712406 -1.1387583 0.82735628 -8.88712406
		 -1.33869064 0.43496695 -8.88712406 -1.40758264 3.9466759e-15 -8.88712406 -1.33869064 -0.43496695 -8.88712406
		 -1.1387583 -0.82735622 -8.88712406 -0.82735622 -1.13875806 -8.88712406 -0.43496695 -1.3386904 -8.88712406
		 -4.1949221e-08 -1.4075824 -8.88712406 0.4349668 -1.33869028 -8.88712406 0.8273561 -1.13875794 -8.88712406
		 1.13875794 -0.82735616 -8.88712406 1.33869028 -0.43496683 -8.88712406 1.40758228 3.9466759e-15 -8.88712406
		 2.64441919 0.85922384 -8.55751419 2.24947739 1.63434076 -8.55751419 1.63434076 2.24947715 -8.55751419
		 0.85922372 2.64441895 -8.55751419 -8.8817842e-16 2.78050637 -8.55751419 -0.85922372 2.64441872 -8.55751419
		 -1.63434041 2.24947643 -8.55751419 -2.24947643 1.63434029 -8.55751419 -2.644418 0.85922348 -8.55751419
		 -2.78050542 3.8002997e-15 -8.55751419 -2.644418 -0.85922348 -8.55751419 -2.24947619 -1.63434017 -8.55751419
		 -1.63434017 -2.24947619 -8.55751419 -0.85922348 -2.64441776 -8.55751419 -8.2865505e-08 -2.78050518 -8.55751419
		 0.85922325 -2.64441752 -8.55751419 1.63433981 -2.24947596 -8.55751419 2.24947572 -1.63433993 -8.55751419
		 2.64441752 -0.85922331 -8.55751419 2.78050494 3.8002997e-15 -8.55751419 3.88503289 1.26232362 -8.01718998
		 3.30480647 2.40108228 -8.01718998 2.40108228 3.30480623 -8.01718998 1.2623235 3.88503218 -8.01718998
		 -8.8817842e-16 4.084964275 -8.01718998 -1.2623235 3.88503194 -8.01718998 -2.4010818 3.30480528 -8.01718998
		 -3.30480504 2.40108156 -8.01718998 -3.88503146 1.26232314 -8.01718998 -4.084963799 3.5603476e-15 -8.01718998
		 -3.88503146 -1.26232314 -8.01718998 -3.3048048 -2.40108132 -8.01718998 -2.40108132 -3.3048048 -8.01718998
		 -1.26232314 -3.88503098 -8.01718998 -1.2174137e-07 -4.084963322 -8.01718998 1.26232278 -3.88503075 -8.01718998
		 2.40108061 -3.30480456 -8.01718998 3.30480433 -2.40108085 -8.01718998 3.88503051 -1.26232302 -8.01718998
		 4.084962368 3.5603476e-15 -8.01718998 5.029983997 1.63434076 -7.27945662 4.27875996 3.10870075 -7.27945662
		 3.10870075 4.27875948 -7.27945662 1.63434064 5.029983521 -7.27945662 -8.8817842e-16 5.28883743 -7.27945662
		 -1.63434064 5.029983044 -7.27945662 -3.10870028 4.27875853 -7.27945662 -4.27875805 3.1086998 -7.27945662
		 -5.029981613 1.63434005 -7.27945662 -5.28883553 3.2327281e-15 -7.27945662 -5.029981613 -1.63434005 -7.27945662
		 -4.27875805 -3.10869956 -7.27945662 -3.10869956 -4.27875757 -7.27945662 -1.63434005 -5.029981136 -7.27945662
		 -1.5761955e-07 -5.28883505 -7.27945662 1.63433969 -5.029981136 -7.27945662 3.10869884 -4.27875757 -7.27945662
		 4.2787571 -3.10869932 -7.27945662 5.029980659 -1.63433981 -7.27945662 5.28883457 3.2327281e-15 -7.27945662
		 6.051080227 1.966115 -6.36247826 5.14735651 3.73977304 -6.36247826 3.73977304 5.14735556 -6.36247826
		 1.96611488 6.05107975 -6.36247826 -8.8817842e-16 6.36248112 -6.36247826 -1.96611488 6.051079273 -6.36247826
		 -3.73977232 5.1473546 -6.36247826 -5.14735413 3.73977184 -6.36247826 -6.051077843 1.96611416 -6.36247826
		 -6.36247969 2.8255079e-15 -6.36247826 -6.051077843 -1.96611416 -6.36247826 -5.14735413 -3.73977137 -6.36247826
		 -3.73977137 -5.14735365 -6.36247826 -1.96611416 -6.051076889 -6.36247826 -1.8961663e-07 -6.36247873 -6.36247826
		 1.96611369 -6.051076889 -6.36247826 3.73977065 -5.14735317 -6.36247826 5.14735317 -3.73977113 -6.36247826
		 6.051076412 -1.96611392 -6.36247826 6.36247826 2.8255079e-15 -6.36247826 6.92317915 2.24947715 -5.28883457
		 5.88920784 4.27875948 -5.28883457 4.27875948 5.88920736 -5.28883457 2.24947667 6.92317772 -5.28883457
		 -8.8817842e-16 7.27945948 -5.28883457 -2.24947667 6.92317724 -5.28883457 -4.278759 5.88920641 -5.28883457
		 -5.88920593 4.27875805 -5.28883457 -6.92317629 2.24947619 -5.28883457 -7.27945805 2.3487144e-15 -5.28883457
		 -6.92317629 -2.24947619 -5.28883457 -5.88920546 -4.27875805 -5.28883457 -4.27875805 -5.8892045 -5.28883457
		 -2.24947619 -6.92317533 -5.28883457 -2.1694471e-07 -7.27945757 -5.28883457 2.24947548 -6.92317533 -5.28883457
		 4.2787571 -5.88920403 -5.28883457 5.88920403 -4.27875757 -5.28883457 6.92317486 -2.24947572 -5.28883457
		 7.27945662 2.3487144e-15 -5.28883457 7.62480593 2.47744942 -4.084962368 6.48604774 4.71238899 -4.084962368
		 4.71238899 6.48604679 -4.084962368 2.47744918 7.62480497 -4.084962368 -8.8817842e-16 8.017193794 -4.084962368
		 -2.47744918 7.62480497 -4.084962368 -4.71238804 6.48604536 -4.084962368 -6.48604488 4.71238708 -4.084962368
		 -7.62480307 2.47744846 -4.084962368 -8.017191887 1.8140877e-15 -4.084962368 -7.62480307 -2.47744846 -4.084962368
		 -6.48604488 -4.71238708 -4.084962368 -4.71238708 -6.48604441 -4.084962368 -2.47744846 -7.62480211 -4.084962368
		 -2.3893088e-07 -8.017190933 -4.084962368 2.47744799 -7.62480164 -4.084962368 4.71238613 -6.48604393 -4.084962368
		 6.48604345 -4.71238661 -4.084962368 7.62480116 -2.47744823 -4.084962368 8.01718998 1.8140877e-15 -4.084962368
		 8.13868523 2.64441895 -2.7805047 6.92317915 5.029983997 -2.7805047 5.029983997 6.92317867 -2.7805047
		 2.64441872 8.13868427 -2.7805047 -8.8817842e-16 8.55751896 -2.7805047 -2.64441872 8.13868332 -2.7805047
		 -5.029983044 6.92317724 -2.7805047 -6.92317677 5.02998209 -2.7805047 -8.13868237 2.644418 -2.7805047
		 -8.5575161 1.2347921e-15 -2.7805047 -8.13868237 -2.644418 -2.7805047 -6.92317629 -5.029981613 -2.7805047
		 -5.029981613 -6.92317581 -2.7805047 -2.644418 -8.13868141 -2.7805047 -2.550338e-07 -8.55751514 -2.7805047
		 2.64441729 -8.13868046 -2.7805047 5.029980659 -6.92317533 -2.7805047 6.92317486 -5.029981136 -2.7805047
		 8.1386795 -2.64441752 -2.7805047 8.55751419 1.2347921e-15 -2.7805047 8.45216274 2.74627399 -1.40758133
		 7.18983889 5.22372341 -1.40758133 5.22372341 7.18983841 -1.40758133 2.74627376 8.45216179 -1.40758133
		 -8.8817842e-16 8.88712788 -1.40758133 -2.74627376 8.45216084 -1.40758133;
	setAttr ".vt[166:331]" -5.22372246 7.18983698 -1.40758133 -7.18983603 5.22372198 -1.40758133
		 -8.45215893 2.74627304 -1.40758133 -8.88712597 6.2509168e-16 -1.40758133 -8.45215893 -2.74627304 -1.40758133
		 -7.18983555 -5.2237215 -1.40758133 -5.2237215 -7.18983507 -1.40758133 -2.74627304 -8.45215797 -1.40758133
		 -2.6485694e-07 -8.88712502 -1.40758133 2.74627209 -8.45215797 -1.40758133 5.22372007 -7.18983459 -1.40758133
		 7.18983412 -5.22372103 -1.40758133 8.45215797 -2.74627233 -1.40758133 8.88712406 6.2509168e-16 -1.40758133
		 8.55751991 2.78050661 -2.3179207e-15 7.27946138 5.28883791 -1.2039979e-15 5.28883791 7.27946091 -3.1998374e-16
		 2.78050637 8.55751896 2.4758794e-16 -8.8817842e-16 8.99790668 4.4315959e-16 -2.78050637 8.55751801 2.4758752e-16
		 -5.28883648 7.279459 -3.1998448e-16 -7.27945852 5.288836 -1.2039986e-15 -8.5575161 2.78050542 -2.3179213e-15
		 -8.99790478 7.8886091e-31 -3.5527137e-15 -8.5575161 -2.78050542 -4.787506e-15 -7.27945805 -5.28883553 -5.9014285e-15
		 -5.28883553 -7.27945757 -6.785442e-15 -2.78050542 -8.55751514 -7.3530142e-15 -2.681584e-07 -8.99790382 -7.5485857e-15
		 2.7805047 -8.55751514 -7.3530142e-15 5.28883457 -7.27945709 -6.785442e-15 7.27945662 -5.28883505 -5.9014285e-15
		 8.55751419 -2.78050494 -4.7875056e-15 8.99790287 7.8886091e-31 -3.5527137e-15 8.45216274 2.74627399 1.40758133
		 7.18983889 5.22372341 1.40758133 5.22372341 7.18983841 1.40758133 2.74627376 8.45216179 1.40758133
		 -8.8817842e-16 8.88712788 1.40758133 -2.74627376 8.45216084 1.40758133 -5.22372246 7.18983698 1.40758133
		 -7.18983603 5.22372198 1.40758133 -8.45215893 2.74627304 1.40758133 -8.88712597 -6.2509168e-16 1.40758133
		 -8.45215893 -2.74627304 1.40758133 -7.18983555 -5.2237215 1.40758133 -5.2237215 -7.18983507 1.40758133
		 -2.74627304 -8.45215797 1.40758133 -2.6485694e-07 -8.88712502 1.40758133 2.74627209 -8.45215797 1.40758133
		 5.22372007 -7.18983459 1.40758133 7.18983412 -5.22372103 1.40758133 8.45215797 -2.74627233 1.40758133
		 8.88712406 -6.2509168e-16 1.40758133 8.13868523 2.64441895 2.7805047 6.92317915 5.029983997 2.7805047
		 5.029983997 6.92317867 2.7805047 2.64441872 8.13868427 2.7805047 -8.8817842e-16 8.55751896 2.7805047
		 -2.64441872 8.13868332 2.7805047 -5.029983044 6.92317724 2.7805047 -6.92317677 5.02998209 2.7805047
		 -8.13868237 2.644418 2.7805047 -8.5575161 -1.2347921e-15 2.7805047 -8.13868237 -2.644418 2.7805047
		 -6.92317629 -5.029981613 2.7805047 -5.029981613 -6.92317581 2.7805047 -2.644418 -8.13868141 2.7805047
		 -2.550338e-07 -8.55751514 2.7805047 2.64441729 -8.13868046 2.7805047 5.029980659 -6.92317533 2.7805047
		 6.92317486 -5.029981136 2.7805047 8.1386795 -2.64441752 2.7805047 8.55751419 -1.2347921e-15 2.7805047
		 7.62480593 2.47744942 4.084962368 6.48604774 4.71238899 4.084962368 4.71238899 6.48604679 4.084962368
		 2.47744918 7.62480497 4.084962368 -8.8817842e-16 8.017193794 4.084962368 -2.47744918 7.62480497 4.084962368
		 -4.71238804 6.48604536 4.084962368 -6.48604488 4.71238708 4.084962368 -7.62480307 2.47744846 4.084962368
		 -8.017191887 -1.8140877e-15 4.084962368 -7.62480307 -2.47744846 4.084962368 -6.48604488 -4.71238708 4.084962368
		 -4.71238708 -6.48604441 4.084962368 -2.47744846 -7.62480211 4.084962368 -2.3893088e-07 -8.017190933 4.084962368
		 2.47744799 -7.62480164 4.084962368 4.71238613 -6.48604393 4.084962368 6.48604345 -4.71238661 4.084962368
		 7.62480116 -2.47744823 4.084962368 8.01718998 -1.8140877e-15 4.084962368 6.92317915 2.24947715 5.28883457
		 5.88920784 4.27875948 5.28883457 4.27875948 5.88920736 5.28883457 2.24947667 6.92317772 5.28883457
		 -8.8817842e-16 7.27945948 5.28883457 -2.24947667 6.92317724 5.28883457 -4.278759 5.88920641 5.28883457
		 -5.88920593 4.27875805 5.28883457 -6.92317629 2.24947619 5.28883457 -7.27945805 -2.3487144e-15 5.28883457
		 -6.92317629 -2.24947619 5.28883457 -5.88920546 -4.27875805 5.28883457 -4.27875805 -5.8892045 5.28883457
		 -2.24947619 -6.92317533 5.28883457 -2.1694471e-07 -7.27945757 5.28883457 2.24947548 -6.92317533 5.28883457
		 4.2787571 -5.88920403 5.28883457 5.88920403 -4.27875757 5.28883457 6.92317486 -2.24947572 5.28883457
		 7.27945662 -2.3487144e-15 5.28883457 6.051080227 1.966115 6.36247826 5.14735651 3.73977304 6.36247826
		 3.73977304 5.14735556 6.36247826 1.96611488 6.05107975 6.36247826 -8.8817842e-16 6.36248112 6.36247826
		 -1.96611488 6.051079273 6.36247826 -3.73977232 5.1473546 6.36247826 -5.14735413 3.73977184 6.36247826
		 -6.051077843 1.96611416 6.36247826 -6.36247969 -2.8255079e-15 6.36247826 -6.051077843 -1.96611416 6.36247826
		 -5.14735413 -3.73977137 6.36247826 -3.73977137 -5.14735365 6.36247826 -1.96611416 -6.051076889 6.36247826
		 -1.8961663e-07 -6.36247873 6.36247826 1.96611369 -6.051076889 6.36247826 3.73977065 -5.14735317 6.36247826
		 5.14735317 -3.73977113 6.36247826 6.051076412 -1.96611392 6.36247826 6.36247826 -2.8255079e-15 6.36247826
		 5.029983997 1.63434076 7.27945662 4.27875996 3.10870075 7.27945662 3.10870075 4.27875948 7.27945662
		 1.63434064 5.029983521 7.27945662 -8.8817842e-16 5.28883743 7.27945662 -1.63434064 5.029983044 7.27945662
		 -3.10870028 4.27875853 7.27945662 -4.27875805 3.1086998 7.27945662 -5.029981613 1.63434005 7.27945662
		 -5.28883553 -3.2327281e-15 7.27945662 -5.029981613 -1.63434005 7.27945662 -4.27875805 -3.10869956 7.27945662
		 -3.10869956 -4.27875757 7.27945662 -1.63434005 -5.029981136 7.27945662 -1.5761955e-07 -5.28883505 7.27945662
		 1.63433969 -5.029981136 7.27945662 3.10869884 -4.27875757 7.27945662 4.2787571 -3.10869932 7.27945662
		 5.029980659 -1.63433981 7.27945662 5.28883457 -3.2327281e-15 7.27945662 3.88503289 1.26232362 8.01718998
		 3.30480647 2.40108228 8.01718998 2.40108228 3.30480623 8.01718998 1.2623235 3.88503218 8.01718998
		 -8.8817842e-16 4.084964275 8.01718998 -1.2623235 3.88503194 8.01718998 -2.4010818 3.30480528 8.01718998
		 -3.30480504 2.40108156 8.01718998 -3.88503146 1.26232314 8.01718998 -4.084963799 -3.5603476e-15 8.01718998
		 -3.88503146 -1.26232314 8.01718998 -3.3048048 -2.40108132 8.01718998;
	setAttr ".vt[332:381]" -2.40108132 -3.3048048 8.01718998 -1.26232314 -3.88503098 8.01718998
		 -1.2174137e-07 -4.084963322 8.01718998 1.26232278 -3.88503075 8.01718998 2.40108061 -3.30480456 8.01718998
		 3.30480433 -2.40108085 8.01718998 3.88503051 -1.26232302 8.01718998 4.084962368 -3.5603476e-15 8.01718998
		 2.64441919 0.85922384 8.55751419 2.24947739 1.63434076 8.55751419 1.63434076 2.24947715 8.55751419
		 0.85922372 2.64441895 8.55751419 -8.8817842e-16 2.78050637 8.55751419 -0.85922372 2.64441872 8.55751419
		 -1.63434041 2.24947643 8.55751419 -2.24947643 1.63434029 8.55751419 -2.644418 0.85922348 8.55751419
		 -2.78050542 -3.8002997e-15 8.55751419 -2.644418 -0.85922348 8.55751419 -2.24947619 -1.63434017 8.55751419
		 -1.63434017 -2.24947619 8.55751419 -0.85922348 -2.64441776 8.55751419 -8.2865505e-08 -2.78050518 8.55751419
		 0.85922325 -2.64441752 8.55751419 1.63433981 -2.24947596 8.55751419 2.24947572 -1.63433993 8.55751419
		 2.64441752 -0.85922331 8.55751419 2.78050494 -3.8002997e-15 8.55751419 1.33869112 0.4349671 8.88712406
		 1.13875866 0.82735658 8.88712406 0.82735658 1.13875866 8.88712406 0.43496704 1.338691 8.88712406
		 -8.8817842e-16 1.40758288 8.88712406 -0.43496704 1.33869088 8.88712406 -0.8273564 1.13875842 8.88712406
		 -1.1387583 0.82735628 8.88712406 -1.33869064 0.43496695 8.88712406 -1.40758264 -3.9466759e-15 8.88712406
		 -1.33869064 -0.43496695 8.88712406 -1.1387583 -0.82735622 8.88712406 -0.82735622 -1.13875806 8.88712406
		 -0.43496695 -1.3386904 8.88712406 -4.1949221e-08 -1.4075824 8.88712406 0.4349668 -1.33869028 8.88712406
		 0.8273561 -1.13875794 8.88712406 1.13875794 -0.82735616 8.88712406 1.33869028 -0.43496683 8.88712406
		 1.40758228 -3.9466759e-15 8.88712406 -8.8817842e-16 3.9958716e-15 -8.99790287 -8.8817842e-16 -3.9958716e-15 8.99790287;
	setAttr -s 780 ".ed";
	setAttr ".ed[0:165]"  0 1 1 1 2 1 2 3 1 3 4 1 4 5 1 5 6 1 6 7 1 7 8 1 8 9 1
		 9 10 1 10 11 1 11 12 1 12 13 1 13 14 1 14 15 1 15 16 1 16 17 1 17 18 1 18 19 1 19 0 1
		 20 21 1 21 22 1 22 23 1 23 24 1 24 25 1 25 26 1 26 27 1 27 28 1 28 29 1 29 30 1 30 31 1
		 31 32 1 32 33 1 33 34 1 34 35 1 35 36 1 36 37 1 37 38 1 38 39 1 39 20 1 40 41 1 41 42 1
		 42 43 1 43 44 1 44 45 1 45 46 1 46 47 1 47 48 1 48 49 1 49 50 1 50 51 1 51 52 1 52 53 1
		 53 54 1 54 55 1 55 56 1 56 57 1 57 58 1 58 59 1 59 40 1 60 61 1 61 62 1 62 63 1 63 64 1
		 64 65 1 65 66 1 66 67 1 67 68 1 68 69 1 69 70 1 70 71 1 71 72 1 72 73 1 73 74 1 74 75 1
		 75 76 1 76 77 1 77 78 1 78 79 1 79 60 1 80 81 1 81 82 1 82 83 1 83 84 1 84 85 1 85 86 1
		 86 87 1 87 88 1 88 89 1 89 90 1 90 91 1 91 92 1 92 93 1 93 94 1 94 95 1 95 96 1 96 97 1
		 97 98 1 98 99 1 99 80 1 100 101 1 101 102 1 102 103 1 103 104 1 104 105 1 105 106 1
		 106 107 1 107 108 1 108 109 1 109 110 1 110 111 1 111 112 1 112 113 1 113 114 1 114 115 1
		 115 116 1 116 117 1 117 118 1 118 119 1 119 100 1 120 121 1 121 122 1 122 123 1 123 124 1
		 124 125 1 125 126 1 126 127 1 127 128 1 128 129 1 129 130 1 130 131 1 131 132 1 132 133 1
		 133 134 1 134 135 1 135 136 1 136 137 1 137 138 1 138 139 1 139 120 1 140 141 1 141 142 1
		 142 143 1 143 144 1 144 145 1 145 146 1 146 147 1 147 148 1 148 149 1 149 150 1 150 151 1
		 151 152 1 152 153 1 153 154 1 154 155 1 155 156 1 156 157 1 157 158 1 158 159 1 159 140 1
		 160 161 1 161 162 1 162 163 1 163 164 1 164 165 1 165 166 1;
	setAttr ".ed[166:331]" 166 167 1 167 168 1 168 169 1 169 170 1 170 171 1 171 172 1
		 172 173 1 173 174 1 174 175 1 175 176 1 176 177 1 177 178 1 178 179 1 179 160 1 180 181 1
		 181 182 1 182 183 1 183 184 1 184 185 1 185 186 1 186 187 1 187 188 1 188 189 1 189 190 1
		 190 191 1 191 192 1 192 193 1 193 194 1 194 195 1 195 196 1 196 197 1 197 198 1 198 199 1
		 199 180 1 200 201 1 201 202 1 202 203 1 203 204 1 204 205 1 205 206 1 206 207 1 207 208 1
		 208 209 1 209 210 1 210 211 1 211 212 1 212 213 1 213 214 1 214 215 1 215 216 1 216 217 1
		 217 218 1 218 219 1 219 200 1 220 221 1 221 222 1 222 223 1 223 224 1 224 225 1 225 226 1
		 226 227 1 227 228 1 228 229 1 229 230 1 230 231 1 231 232 1 232 233 1 233 234 1 234 235 1
		 235 236 1 236 237 1 237 238 1 238 239 1 239 220 1 240 241 1 241 242 1 242 243 1 243 244 1
		 244 245 1 245 246 1 246 247 1 247 248 1 248 249 1 249 250 1 250 251 1 251 252 1 252 253 1
		 253 254 1 254 255 1 255 256 1 256 257 1 257 258 1 258 259 1 259 240 1 260 261 1 261 262 1
		 262 263 1 263 264 1 264 265 1 265 266 1 266 267 1 267 268 1 268 269 1 269 270 1 270 271 1
		 271 272 1 272 273 1 273 274 1 274 275 1 275 276 1 276 277 1 277 278 1 278 279 1 279 260 1
		 280 281 1 281 282 1 282 283 1 283 284 1 284 285 1 285 286 1 286 287 1 287 288 1 288 289 1
		 289 290 1 290 291 1 291 292 1 292 293 1 293 294 1 294 295 1 295 296 1 296 297 1 297 298 1
		 298 299 1 299 280 1 300 301 1 301 302 1 302 303 1 303 304 1 304 305 1 305 306 1 306 307 1
		 307 308 1 308 309 1 309 310 1 310 311 1 311 312 1 312 313 1 313 314 1 314 315 1 315 316 1
		 316 317 1 317 318 1 318 319 1 319 300 1 320 321 1 321 322 1 322 323 1 323 324 1 324 325 1
		 325 326 1 326 327 1 327 328 1 328 329 1 329 330 1 330 331 1 331 332 1;
	setAttr ".ed[332:497]" 332 333 1 333 334 1 334 335 1 335 336 1 336 337 1 337 338 1
		 338 339 1 339 320 1 340 341 1 341 342 1 342 343 1 343 344 1 344 345 1 345 346 1 346 347 1
		 347 348 1 348 349 1 349 350 1 350 351 1 351 352 1 352 353 1 353 354 1 354 355 1 355 356 1
		 356 357 1 357 358 1 358 359 1 359 340 1 360 361 1 361 362 1 362 363 1 363 364 1 364 365 1
		 365 366 1 366 367 1 367 368 1 368 369 1 369 370 1 370 371 1 371 372 1 372 373 1 373 374 1
		 374 375 1 375 376 1 376 377 1 377 378 1 378 379 1 379 360 1 0 20 1 1 21 1 2 22 1
		 3 23 1 4 24 1 5 25 1 6 26 1 7 27 1 8 28 1 9 29 1 10 30 1 11 31 1 12 32 1 13 33 1
		 14 34 1 15 35 1 16 36 1 17 37 1 18 38 1 19 39 1 20 40 1 21 41 1 22 42 1 23 43 1 24 44 1
		 25 45 1 26 46 1 27 47 1 28 48 1 29 49 1 30 50 1 31 51 1 32 52 1 33 53 1 34 54 1 35 55 1
		 36 56 1 37 57 1 38 58 1 39 59 1 40 60 1 41 61 1 42 62 1 43 63 1 44 64 1 45 65 1 46 66 1
		 47 67 1 48 68 1 49 69 1 50 70 1 51 71 1 52 72 1 53 73 1 54 74 1 55 75 1 56 76 1 57 77 1
		 58 78 1 59 79 1 60 80 1 61 81 1 62 82 1 63 83 1 64 84 1 65 85 1 66 86 1 67 87 1 68 88 1
		 69 89 1 70 90 1 71 91 1 72 92 1 73 93 1 74 94 1 75 95 1 76 96 1 77 97 1 78 98 1 79 99 1
		 80 100 1 81 101 1 82 102 1 83 103 1 84 104 1 85 105 1 86 106 1 87 107 1 88 108 1
		 89 109 1 90 110 1 91 111 1 92 112 1 93 113 1 94 114 1 95 115 1 96 116 1 97 117 1
		 98 118 1 99 119 1 100 120 1 101 121 1 102 122 1 103 123 1 104 124 1 105 125 1 106 126 1
		 107 127 1 108 128 1 109 129 1 110 130 1 111 131 1 112 132 1 113 133 1 114 134 1 115 135 1
		 116 136 1 117 137 1;
	setAttr ".ed[498:663]" 118 138 1 119 139 1 120 140 1 121 141 1 122 142 1 123 143 1
		 124 144 1 125 145 1 126 146 1 127 147 1 128 148 1 129 149 1 130 150 1 131 151 1 132 152 1
		 133 153 1 134 154 1 135 155 1 136 156 1 137 157 1 138 158 1 139 159 1 140 160 1 141 161 1
		 142 162 1 143 163 1 144 164 1 145 165 1 146 166 1 147 167 1 148 168 1 149 169 1 150 170 1
		 151 171 1 152 172 1 153 173 1 154 174 1 155 175 1 156 176 1 157 177 1 158 178 1 159 179 1
		 160 180 1 161 181 1 162 182 1 163 183 1 164 184 1 165 185 1 166 186 1 167 187 1 168 188 1
		 169 189 1 170 190 1 171 191 1 172 192 1 173 193 1 174 194 1 175 195 1 176 196 1 177 197 1
		 178 198 1 179 199 1 180 200 1 181 201 1 182 202 1 183 203 1 184 204 1 185 205 1 186 206 1
		 187 207 1 188 208 1 189 209 1 190 210 1 191 211 1 192 212 1 193 213 1 194 214 1 195 215 1
		 196 216 1 197 217 1 198 218 1 199 219 1 200 220 1 201 221 1 202 222 1 203 223 1 204 224 1
		 205 225 1 206 226 1 207 227 1 208 228 1 209 229 1 210 230 1 211 231 1 212 232 1 213 233 1
		 214 234 1 215 235 1 216 236 1 217 237 1 218 238 1 219 239 1 220 240 1 221 241 1 222 242 1
		 223 243 1 224 244 1 225 245 1 226 246 1 227 247 1 228 248 1 229 249 1 230 250 1 231 251 1
		 232 252 1 233 253 1 234 254 1 235 255 1 236 256 1 237 257 1 238 258 1 239 259 1 240 260 1
		 241 261 1 242 262 1 243 263 1 244 264 1 245 265 1 246 266 1 247 267 1 248 268 1 249 269 1
		 250 270 1 251 271 1 252 272 1 253 273 1 254 274 1 255 275 1 256 276 1 257 277 1 258 278 1
		 259 279 1 260 280 1 261 281 1 262 282 1 263 283 1 264 284 1 265 285 1 266 286 1 267 287 1
		 268 288 1 269 289 1 270 290 1 271 291 1 272 292 1 273 293 1 274 294 1 275 295 1 276 296 1
		 277 297 1 278 298 1 279 299 1 280 300 1 281 301 1 282 302 1 283 303 1;
	setAttr ".ed[664:779]" 284 304 1 285 305 1 286 306 1 287 307 1 288 308 1 289 309 1
		 290 310 1 291 311 1 292 312 1 293 313 1 294 314 1 295 315 1 296 316 1 297 317 1 298 318 1
		 299 319 1 300 320 1 301 321 1 302 322 1 303 323 1 304 324 1 305 325 1 306 326 1 307 327 1
		 308 328 1 309 329 1 310 330 1 311 331 1 312 332 1 313 333 1 314 334 1 315 335 1 316 336 1
		 317 337 1 318 338 1 319 339 1 320 340 1 321 341 1 322 342 1 323 343 1 324 344 1 325 345 1
		 326 346 1 327 347 1 328 348 1 329 349 1 330 350 1 331 351 1 332 352 1 333 353 1 334 354 1
		 335 355 1 336 356 1 337 357 1 338 358 1 339 359 1 340 360 1 341 361 1 342 362 1 343 363 1
		 344 364 1 345 365 1 346 366 1 347 367 1 348 368 1 349 369 1 350 370 1 351 371 1 352 372 1
		 353 373 1 354 374 1 355 375 1 356 376 1 357 377 1 358 378 1 359 379 1 380 0 1 380 1 1
		 380 2 1 380 3 1 380 4 1 380 5 1 380 6 1 380 7 1 380 8 1 380 9 1 380 10 1 380 11 1
		 380 12 1 380 13 1 380 14 1 380 15 1 380 16 1 380 17 1 380 18 1 380 19 1 360 381 1
		 361 381 1 362 381 1 363 381 1 364 381 1 365 381 1 366 381 1 367 381 1 368 381 1 369 381 1
		 370 381 1 371 381 1 372 381 1 373 381 1 374 381 1 375 381 1 376 381 1 377 381 1 378 381 1
		 379 381 1;
	setAttr -s 400 -ch 1560 ".fc[0:399]" -type "polyFaces" 
		f 4 0 381 -21 -381
		mu 0 4 0 1 22 21
		f 4 1 382 -22 -382
		mu 0 4 1 2 23 22
		f 4 2 383 -23 -383
		mu 0 4 2 3 24 23
		f 4 3 384 -24 -384
		mu 0 4 3 4 25 24
		f 4 4 385 -25 -385
		mu 0 4 4 5 26 25
		f 4 5 386 -26 -386
		mu 0 4 5 6 27 26
		f 4 6 387 -27 -387
		mu 0 4 6 7 28 27
		f 4 7 388 -28 -388
		mu 0 4 7 8 29 28
		f 4 8 389 -29 -389
		mu 0 4 8 9 30 29
		f 4 9 390 -30 -390
		mu 0 4 9 10 31 30
		f 4 10 391 -31 -391
		mu 0 4 10 11 32 31
		f 4 11 392 -32 -392
		mu 0 4 11 12 33 32
		f 4 12 393 -33 -393
		mu 0 4 12 13 34 33
		f 4 13 394 -34 -394
		mu 0 4 13 14 35 34
		f 4 14 395 -35 -395
		mu 0 4 14 15 36 35
		f 4 15 396 -36 -396
		mu 0 4 15 16 37 36
		f 4 16 397 -37 -397
		mu 0 4 16 17 38 37
		f 4 17 398 -38 -398
		mu 0 4 17 18 39 38
		f 4 18 399 -39 -399
		mu 0 4 18 19 40 39
		f 4 19 380 -40 -400
		mu 0 4 19 20 41 40
		f 4 20 401 -41 -401
		mu 0 4 21 22 43 42
		f 4 21 402 -42 -402
		mu 0 4 22 23 44 43
		f 4 22 403 -43 -403
		mu 0 4 23 24 45 44
		f 4 23 404 -44 -404
		mu 0 4 24 25 46 45
		f 4 24 405 -45 -405
		mu 0 4 25 26 47 46
		f 4 25 406 -46 -406
		mu 0 4 26 27 48 47
		f 4 26 407 -47 -407
		mu 0 4 27 28 49 48
		f 4 27 408 -48 -408
		mu 0 4 28 29 50 49
		f 4 28 409 -49 -409
		mu 0 4 29 30 51 50
		f 4 29 410 -50 -410
		mu 0 4 30 31 52 51
		f 4 30 411 -51 -411
		mu 0 4 31 32 53 52
		f 4 31 412 -52 -412
		mu 0 4 32 33 54 53
		f 4 32 413 -53 -413
		mu 0 4 33 34 55 54
		f 4 33 414 -54 -414
		mu 0 4 34 35 56 55
		f 4 34 415 -55 -415
		mu 0 4 35 36 57 56
		f 4 35 416 -56 -416
		mu 0 4 36 37 58 57
		f 4 36 417 -57 -417
		mu 0 4 37 38 59 58
		f 4 37 418 -58 -418
		mu 0 4 38 39 60 59
		f 4 38 419 -59 -419
		mu 0 4 39 40 61 60
		f 4 39 400 -60 -420
		mu 0 4 40 41 62 61
		f 4 40 421 -61 -421
		mu 0 4 42 43 64 63
		f 4 41 422 -62 -422
		mu 0 4 43 44 65 64
		f 4 42 423 -63 -423
		mu 0 4 44 45 66 65
		f 4 43 424 -64 -424
		mu 0 4 45 46 67 66
		f 4 44 425 -65 -425
		mu 0 4 46 47 68 67
		f 4 45 426 -66 -426
		mu 0 4 47 48 69 68
		f 4 46 427 -67 -427
		mu 0 4 48 49 70 69
		f 4 47 428 -68 -428
		mu 0 4 49 50 71 70
		f 4 48 429 -69 -429
		mu 0 4 50 51 72 71
		f 4 49 430 -70 -430
		mu 0 4 51 52 73 72
		f 4 50 431 -71 -431
		mu 0 4 52 53 74 73
		f 4 51 432 -72 -432
		mu 0 4 53 54 75 74
		f 4 52 433 -73 -433
		mu 0 4 54 55 76 75
		f 4 53 434 -74 -434
		mu 0 4 55 56 77 76
		f 4 54 435 -75 -435
		mu 0 4 56 57 78 77
		f 4 55 436 -76 -436
		mu 0 4 57 58 79 78
		f 4 56 437 -77 -437
		mu 0 4 58 59 80 79
		f 4 57 438 -78 -438
		mu 0 4 59 60 81 80
		f 4 58 439 -79 -439
		mu 0 4 60 61 82 81
		f 4 59 420 -80 -440
		mu 0 4 61 62 83 82
		f 4 60 441 -81 -441
		mu 0 4 63 64 85 84
		f 4 61 442 -82 -442
		mu 0 4 64 65 86 85
		f 4 62 443 -83 -443
		mu 0 4 65 66 87 86
		f 4 63 444 -84 -444
		mu 0 4 66 67 88 87
		f 4 64 445 -85 -445
		mu 0 4 67 68 89 88
		f 4 65 446 -86 -446
		mu 0 4 68 69 90 89
		f 4 66 447 -87 -447
		mu 0 4 69 70 91 90
		f 4 67 448 -88 -448
		mu 0 4 70 71 92 91
		f 4 68 449 -89 -449
		mu 0 4 71 72 93 92
		f 4 69 450 -90 -450
		mu 0 4 72 73 94 93
		f 4 70 451 -91 -451
		mu 0 4 73 74 95 94
		f 4 71 452 -92 -452
		mu 0 4 74 75 96 95
		f 4 72 453 -93 -453
		mu 0 4 75 76 97 96
		f 4 73 454 -94 -454
		mu 0 4 76 77 98 97
		f 4 74 455 -95 -455
		mu 0 4 77 78 99 98
		f 4 75 456 -96 -456
		mu 0 4 78 79 100 99
		f 4 76 457 -97 -457
		mu 0 4 79 80 101 100
		f 4 77 458 -98 -458
		mu 0 4 80 81 102 101
		f 4 78 459 -99 -459
		mu 0 4 81 82 103 102
		f 4 79 440 -100 -460
		mu 0 4 82 83 104 103
		f 4 80 461 -101 -461
		mu 0 4 84 85 106 105
		f 4 81 462 -102 -462
		mu 0 4 85 86 107 106
		f 4 82 463 -103 -463
		mu 0 4 86 87 108 107
		f 4 83 464 -104 -464
		mu 0 4 87 88 109 108
		f 4 84 465 -105 -465
		mu 0 4 88 89 110 109
		f 4 85 466 -106 -466
		mu 0 4 89 90 111 110
		f 4 86 467 -107 -467
		mu 0 4 90 91 112 111
		f 4 87 468 -108 -468
		mu 0 4 91 92 113 112
		f 4 88 469 -109 -469
		mu 0 4 92 93 114 113
		f 4 89 470 -110 -470
		mu 0 4 93 94 115 114
		f 4 90 471 -111 -471
		mu 0 4 94 95 116 115
		f 4 91 472 -112 -472
		mu 0 4 95 96 117 116
		f 4 92 473 -113 -473
		mu 0 4 96 97 118 117
		f 4 93 474 -114 -474
		mu 0 4 97 98 119 118
		f 4 94 475 -115 -475
		mu 0 4 98 99 120 119
		f 4 95 476 -116 -476
		mu 0 4 99 100 121 120
		f 4 96 477 -117 -477
		mu 0 4 100 101 122 121
		f 4 97 478 -118 -478
		mu 0 4 101 102 123 122
		f 4 98 479 -119 -479
		mu 0 4 102 103 124 123
		f 4 99 460 -120 -480
		mu 0 4 103 104 125 124
		f 4 100 481 -121 -481
		mu 0 4 105 106 127 126
		f 4 101 482 -122 -482
		mu 0 4 106 107 128 127
		f 4 102 483 -123 -483
		mu 0 4 107 108 129 128
		f 4 103 484 -124 -484
		mu 0 4 108 109 130 129
		f 4 104 485 -125 -485
		mu 0 4 109 110 131 130
		f 4 105 486 -126 -486
		mu 0 4 110 111 132 131
		f 4 106 487 -127 -487
		mu 0 4 111 112 133 132
		f 4 107 488 -128 -488
		mu 0 4 112 113 134 133
		f 4 108 489 -129 -489
		mu 0 4 113 114 135 134
		f 4 109 490 -130 -490
		mu 0 4 114 115 136 135
		f 4 110 491 -131 -491
		mu 0 4 115 116 137 136
		f 4 111 492 -132 -492
		mu 0 4 116 117 138 137
		f 4 112 493 -133 -493
		mu 0 4 117 118 139 138
		f 4 113 494 -134 -494
		mu 0 4 118 119 140 139
		f 4 114 495 -135 -495
		mu 0 4 119 120 141 140
		f 4 115 496 -136 -496
		mu 0 4 120 121 142 141
		f 4 116 497 -137 -497
		mu 0 4 121 122 143 142
		f 4 117 498 -138 -498
		mu 0 4 122 123 144 143
		f 4 118 499 -139 -499
		mu 0 4 123 124 145 144
		f 4 119 480 -140 -500
		mu 0 4 124 125 146 145
		f 4 120 501 -141 -501
		mu 0 4 126 127 148 147
		f 4 121 502 -142 -502
		mu 0 4 127 128 149 148
		f 4 122 503 -143 -503
		mu 0 4 128 129 150 149
		f 4 123 504 -144 -504
		mu 0 4 129 130 151 150
		f 4 124 505 -145 -505
		mu 0 4 130 131 152 151
		f 4 125 506 -146 -506
		mu 0 4 131 132 153 152
		f 4 126 507 -147 -507
		mu 0 4 132 133 154 153
		f 4 127 508 -148 -508
		mu 0 4 133 134 155 154
		f 4 128 509 -149 -509
		mu 0 4 134 135 156 155
		f 4 129 510 -150 -510
		mu 0 4 135 136 157 156
		f 4 130 511 -151 -511
		mu 0 4 136 137 158 157
		f 4 131 512 -152 -512
		mu 0 4 137 138 159 158
		f 4 132 513 -153 -513
		mu 0 4 138 139 160 159
		f 4 133 514 -154 -514
		mu 0 4 139 140 161 160
		f 4 134 515 -155 -515
		mu 0 4 140 141 162 161
		f 4 135 516 -156 -516
		mu 0 4 141 142 163 162
		f 4 136 517 -157 -517
		mu 0 4 142 143 164 163
		f 4 137 518 -158 -518
		mu 0 4 143 144 165 164
		f 4 138 519 -159 -519
		mu 0 4 144 145 166 165
		f 4 139 500 -160 -520
		mu 0 4 145 146 167 166
		f 4 140 521 -161 -521
		mu 0 4 147 148 169 168
		f 4 141 522 -162 -522
		mu 0 4 148 149 170 169
		f 4 142 523 -163 -523
		mu 0 4 149 150 171 170
		f 4 143 524 -164 -524
		mu 0 4 150 151 172 171
		f 4 144 525 -165 -525
		mu 0 4 151 152 173 172
		f 4 145 526 -166 -526
		mu 0 4 152 153 174 173
		f 4 146 527 -167 -527
		mu 0 4 153 154 175 174
		f 4 147 528 -168 -528
		mu 0 4 154 155 176 175
		f 4 148 529 -169 -529
		mu 0 4 155 156 177 176
		f 4 149 530 -170 -530
		mu 0 4 156 157 178 177
		f 4 150 531 -171 -531
		mu 0 4 157 158 179 178
		f 4 151 532 -172 -532
		mu 0 4 158 159 180 179
		f 4 152 533 -173 -533
		mu 0 4 159 160 181 180
		f 4 153 534 -174 -534
		mu 0 4 160 161 182 181
		f 4 154 535 -175 -535
		mu 0 4 161 162 183 182
		f 4 155 536 -176 -536
		mu 0 4 162 163 184 183
		f 4 156 537 -177 -537
		mu 0 4 163 164 185 184
		f 4 157 538 -178 -538
		mu 0 4 164 165 186 185
		f 4 158 539 -179 -539
		mu 0 4 165 166 187 186
		f 4 159 520 -180 -540
		mu 0 4 166 167 188 187
		f 4 160 541 -181 -541
		mu 0 4 168 169 190 189
		f 4 161 542 -182 -542
		mu 0 4 169 170 191 190
		f 4 162 543 -183 -543
		mu 0 4 170 171 192 191
		f 4 163 544 -184 -544
		mu 0 4 171 172 193 192
		f 4 164 545 -185 -545
		mu 0 4 172 173 194 193
		f 4 165 546 -186 -546
		mu 0 4 173 174 195 194
		f 4 166 547 -187 -547
		mu 0 4 174 175 196 195
		f 4 167 548 -188 -548
		mu 0 4 175 176 197 196
		f 4 168 549 -189 -549
		mu 0 4 176 177 198 197
		f 4 169 550 -190 -550
		mu 0 4 177 178 199 198
		f 4 170 551 -191 -551
		mu 0 4 178 179 200 199
		f 4 171 552 -192 -552
		mu 0 4 179 180 201 200
		f 4 172 553 -193 -553
		mu 0 4 180 181 202 201
		f 4 173 554 -194 -554
		mu 0 4 181 182 203 202
		f 4 174 555 -195 -555
		mu 0 4 182 183 204 203
		f 4 175 556 -196 -556
		mu 0 4 183 184 205 204
		f 4 176 557 -197 -557
		mu 0 4 184 185 206 205
		f 4 177 558 -198 -558
		mu 0 4 185 186 207 206
		f 4 178 559 -199 -559
		mu 0 4 186 187 208 207
		f 4 179 540 -200 -560
		mu 0 4 187 188 209 208
		f 4 180 561 -201 -561
		mu 0 4 189 190 211 210
		f 4 181 562 -202 -562
		mu 0 4 190 191 212 211
		f 4 182 563 -203 -563
		mu 0 4 191 192 213 212
		f 4 183 564 -204 -564
		mu 0 4 192 193 214 213
		f 4 184 565 -205 -565
		mu 0 4 193 194 215 214
		f 4 185 566 -206 -566
		mu 0 4 194 195 216 215
		f 4 186 567 -207 -567
		mu 0 4 195 196 217 216
		f 4 187 568 -208 -568
		mu 0 4 196 197 218 217
		f 4 188 569 -209 -569
		mu 0 4 197 198 219 218
		f 4 189 570 -210 -570
		mu 0 4 198 199 220 219
		f 4 190 571 -211 -571
		mu 0 4 199 200 221 220
		f 4 191 572 -212 -572
		mu 0 4 200 201 222 221
		f 4 192 573 -213 -573
		mu 0 4 201 202 223 222
		f 4 193 574 -214 -574
		mu 0 4 202 203 224 223
		f 4 194 575 -215 -575
		mu 0 4 203 204 225 224
		f 4 195 576 -216 -576
		mu 0 4 204 205 226 225
		f 4 196 577 -217 -577
		mu 0 4 205 206 227 226
		f 4 197 578 -218 -578
		mu 0 4 206 207 228 227
		f 4 198 579 -219 -579
		mu 0 4 207 208 229 228
		f 4 199 560 -220 -580
		mu 0 4 208 209 230 229
		f 4 200 581 -221 -581
		mu 0 4 210 211 232 231
		f 4 201 582 -222 -582
		mu 0 4 211 212 233 232
		f 4 202 583 -223 -583
		mu 0 4 212 213 234 233
		f 4 203 584 -224 -584
		mu 0 4 213 214 235 234
		f 4 204 585 -225 -585
		mu 0 4 214 215 236 235
		f 4 205 586 -226 -586
		mu 0 4 215 216 237 236
		f 4 206 587 -227 -587
		mu 0 4 216 217 238 237
		f 4 207 588 -228 -588
		mu 0 4 217 218 239 238
		f 4 208 589 -229 -589
		mu 0 4 218 219 240 239
		f 4 209 590 -230 -590
		mu 0 4 219 220 241 240
		f 4 210 591 -231 -591
		mu 0 4 220 221 242 241
		f 4 211 592 -232 -592
		mu 0 4 221 222 243 242
		f 4 212 593 -233 -593
		mu 0 4 222 223 244 243
		f 4 213 594 -234 -594
		mu 0 4 223 224 245 244
		f 4 214 595 -235 -595
		mu 0 4 224 225 246 245
		f 4 215 596 -236 -596
		mu 0 4 225 226 247 246
		f 4 216 597 -237 -597
		mu 0 4 226 227 248 247
		f 4 217 598 -238 -598
		mu 0 4 227 228 249 248
		f 4 218 599 -239 -599
		mu 0 4 228 229 250 249
		f 4 219 580 -240 -600
		mu 0 4 229 230 251 250
		f 4 220 601 -241 -601
		mu 0 4 231 232 253 252
		f 4 221 602 -242 -602
		mu 0 4 232 233 254 253
		f 4 222 603 -243 -603
		mu 0 4 233 234 255 254
		f 4 223 604 -244 -604
		mu 0 4 234 235 256 255
		f 4 224 605 -245 -605
		mu 0 4 235 236 257 256
		f 4 225 606 -246 -606
		mu 0 4 236 237 258 257
		f 4 226 607 -247 -607
		mu 0 4 237 238 259 258
		f 4 227 608 -248 -608
		mu 0 4 238 239 260 259
		f 4 228 609 -249 -609
		mu 0 4 239 240 261 260
		f 4 229 610 -250 -610
		mu 0 4 240 241 262 261
		f 4 230 611 -251 -611
		mu 0 4 241 242 263 262
		f 4 231 612 -252 -612
		mu 0 4 242 243 264 263
		f 4 232 613 -253 -613
		mu 0 4 243 244 265 264
		f 4 233 614 -254 -614
		mu 0 4 244 245 266 265
		f 4 234 615 -255 -615
		mu 0 4 245 246 267 266
		f 4 235 616 -256 -616
		mu 0 4 246 247 268 267
		f 4 236 617 -257 -617
		mu 0 4 247 248 269 268
		f 4 237 618 -258 -618
		mu 0 4 248 249 270 269
		f 4 238 619 -259 -619
		mu 0 4 249 250 271 270
		f 4 239 600 -260 -620
		mu 0 4 250 251 272 271
		f 4 240 621 -261 -621
		mu 0 4 252 253 274 273
		f 4 241 622 -262 -622
		mu 0 4 253 254 275 274
		f 4 242 623 -263 -623
		mu 0 4 254 255 276 275
		f 4 243 624 -264 -624
		mu 0 4 255 256 277 276
		f 4 244 625 -265 -625
		mu 0 4 256 257 278 277
		f 4 245 626 -266 -626
		mu 0 4 257 258 279 278
		f 4 246 627 -267 -627
		mu 0 4 258 259 280 279
		f 4 247 628 -268 -628
		mu 0 4 259 260 281 280
		f 4 248 629 -269 -629
		mu 0 4 260 261 282 281
		f 4 249 630 -270 -630
		mu 0 4 261 262 283 282
		f 4 250 631 -271 -631
		mu 0 4 262 263 284 283
		f 4 251 632 -272 -632
		mu 0 4 263 264 285 284
		f 4 252 633 -273 -633
		mu 0 4 264 265 286 285
		f 4 253 634 -274 -634
		mu 0 4 265 266 287 286
		f 4 254 635 -275 -635
		mu 0 4 266 267 288 287
		f 4 255 636 -276 -636
		mu 0 4 267 268 289 288
		f 4 256 637 -277 -637
		mu 0 4 268 269 290 289
		f 4 257 638 -278 -638
		mu 0 4 269 270 291 290
		f 4 258 639 -279 -639
		mu 0 4 270 271 292 291
		f 4 259 620 -280 -640
		mu 0 4 271 272 293 292
		f 4 260 641 -281 -641
		mu 0 4 273 274 295 294
		f 4 261 642 -282 -642
		mu 0 4 274 275 296 295
		f 4 262 643 -283 -643
		mu 0 4 275 276 297 296
		f 4 263 644 -284 -644
		mu 0 4 276 277 298 297
		f 4 264 645 -285 -645
		mu 0 4 277 278 299 298
		f 4 265 646 -286 -646
		mu 0 4 278 279 300 299
		f 4 266 647 -287 -647
		mu 0 4 279 280 301 300
		f 4 267 648 -288 -648
		mu 0 4 280 281 302 301
		f 4 268 649 -289 -649
		mu 0 4 281 282 303 302
		f 4 269 650 -290 -650
		mu 0 4 282 283 304 303
		f 4 270 651 -291 -651
		mu 0 4 283 284 305 304
		f 4 271 652 -292 -652
		mu 0 4 284 285 306 305
		f 4 272 653 -293 -653
		mu 0 4 285 286 307 306
		f 4 273 654 -294 -654
		mu 0 4 286 287 308 307
		f 4 274 655 -295 -655
		mu 0 4 287 288 309 308
		f 4 275 656 -296 -656
		mu 0 4 288 289 310 309
		f 4 276 657 -297 -657
		mu 0 4 289 290 311 310
		f 4 277 658 -298 -658
		mu 0 4 290 291 312 311
		f 4 278 659 -299 -659
		mu 0 4 291 292 313 312
		f 4 279 640 -300 -660
		mu 0 4 292 293 314 313
		f 4 280 661 -301 -661
		mu 0 4 294 295 316 315
		f 4 281 662 -302 -662
		mu 0 4 295 296 317 316
		f 4 282 663 -303 -663
		mu 0 4 296 297 318 317
		f 4 283 664 -304 -664
		mu 0 4 297 298 319 318
		f 4 284 665 -305 -665
		mu 0 4 298 299 320 319
		f 4 285 666 -306 -666
		mu 0 4 299 300 321 320
		f 4 286 667 -307 -667
		mu 0 4 300 301 322 321
		f 4 287 668 -308 -668
		mu 0 4 301 302 323 322
		f 4 288 669 -309 -669
		mu 0 4 302 303 324 323
		f 4 289 670 -310 -670
		mu 0 4 303 304 325 324
		f 4 290 671 -311 -671
		mu 0 4 304 305 326 325
		f 4 291 672 -312 -672
		mu 0 4 305 306 327 326
		f 4 292 673 -313 -673
		mu 0 4 306 307 328 327
		f 4 293 674 -314 -674
		mu 0 4 307 308 329 328
		f 4 294 675 -315 -675
		mu 0 4 308 309 330 329
		f 4 295 676 -316 -676
		mu 0 4 309 310 331 330
		f 4 296 677 -317 -677
		mu 0 4 310 311 332 331
		f 4 297 678 -318 -678
		mu 0 4 311 312 333 332
		f 4 298 679 -319 -679
		mu 0 4 312 313 334 333
		f 4 299 660 -320 -680
		mu 0 4 313 314 335 334
		f 4 300 681 -321 -681
		mu 0 4 315 316 337 336
		f 4 301 682 -322 -682
		mu 0 4 316 317 338 337
		f 4 302 683 -323 -683
		mu 0 4 317 318 339 338
		f 4 303 684 -324 -684
		mu 0 4 318 319 340 339
		f 4 304 685 -325 -685
		mu 0 4 319 320 341 340
		f 4 305 686 -326 -686
		mu 0 4 320 321 342 341
		f 4 306 687 -327 -687
		mu 0 4 321 322 343 342
		f 4 307 688 -328 -688
		mu 0 4 322 323 344 343
		f 4 308 689 -329 -689
		mu 0 4 323 324 345 344
		f 4 309 690 -330 -690
		mu 0 4 324 325 346 345
		f 4 310 691 -331 -691
		mu 0 4 325 326 347 346
		f 4 311 692 -332 -692
		mu 0 4 326 327 348 347
		f 4 312 693 -333 -693
		mu 0 4 327 328 349 348
		f 4 313 694 -334 -694
		mu 0 4 328 329 350 349
		f 4 314 695 -335 -695
		mu 0 4 329 330 351 350
		f 4 315 696 -336 -696
		mu 0 4 330 331 352 351
		f 4 316 697 -337 -697
		mu 0 4 331 332 353 352
		f 4 317 698 -338 -698
		mu 0 4 332 333 354 353
		f 4 318 699 -339 -699
		mu 0 4 333 334 355 354
		f 4 319 680 -340 -700
		mu 0 4 334 335 356 355
		f 4 320 701 -341 -701
		mu 0 4 336 337 358 357
		f 4 321 702 -342 -702
		mu 0 4 337 338 359 358
		f 4 322 703 -343 -703
		mu 0 4 338 339 360 359
		f 4 323 704 -344 -704
		mu 0 4 339 340 361 360
		f 4 324 705 -345 -705
		mu 0 4 340 341 362 361
		f 4 325 706 -346 -706
		mu 0 4 341 342 363 362
		f 4 326 707 -347 -707
		mu 0 4 342 343 364 363
		f 4 327 708 -348 -708
		mu 0 4 343 344 365 364
		f 4 328 709 -349 -709
		mu 0 4 344 345 366 365
		f 4 329 710 -350 -710
		mu 0 4 345 346 367 366
		f 4 330 711 -351 -711
		mu 0 4 346 347 368 367
		f 4 331 712 -352 -712
		mu 0 4 347 348 369 368
		f 4 332 713 -353 -713
		mu 0 4 348 349 370 369
		f 4 333 714 -354 -714
		mu 0 4 349 350 371 370
		f 4 334 715 -355 -715
		mu 0 4 350 351 372 371
		f 4 335 716 -356 -716
		mu 0 4 351 352 373 372
		f 4 336 717 -357 -717
		mu 0 4 352 353 374 373
		f 4 337 718 -358 -718
		mu 0 4 353 354 375 374
		f 4 338 719 -359 -719
		mu 0 4 354 355 376 375
		f 4 339 700 -360 -720
		mu 0 4 355 356 377 376
		f 4 340 721 -361 -721
		mu 0 4 357 358 379 378
		f 4 341 722 -362 -722
		mu 0 4 358 359 380 379
		f 4 342 723 -363 -723
		mu 0 4 359 360 381 380
		f 4 343 724 -364 -724
		mu 0 4 360 361 382 381
		f 4 344 725 -365 -725
		mu 0 4 361 362 383 382
		f 4 345 726 -366 -726
		mu 0 4 362 363 384 383
		f 4 346 727 -367 -727
		mu 0 4 363 364 385 384
		f 4 347 728 -368 -728
		mu 0 4 364 365 386 385
		f 4 348 729 -369 -729
		mu 0 4 365 366 387 386
		f 4 349 730 -370 -730
		mu 0 4 366 367 388 387
		f 4 350 731 -371 -731
		mu 0 4 367 368 389 388
		f 4 351 732 -372 -732
		mu 0 4 368 369 390 389
		f 4 352 733 -373 -733
		mu 0 4 369 370 391 390
		f 4 353 734 -374 -734
		mu 0 4 370 371 392 391
		f 4 354 735 -375 -735
		mu 0 4 371 372 393 392
		f 4 355 736 -376 -736
		mu 0 4 372 373 394 393
		f 4 356 737 -377 -737
		mu 0 4 373 374 395 394
		f 4 357 738 -378 -738
		mu 0 4 374 375 396 395
		f 4 358 739 -379 -739
		mu 0 4 375 376 397 396
		f 4 359 720 -380 -740
		mu 0 4 376 377 398 397
		f 3 -1 -741 741
		mu 0 3 1 0 399
		f 3 -2 -742 742
		mu 0 3 2 1 400
		f 3 -3 -743 743
		mu 0 3 3 2 401
		f 3 -4 -744 744
		mu 0 3 4 3 402
		f 3 -5 -745 745
		mu 0 3 5 4 403
		f 3 -6 -746 746
		mu 0 3 6 5 404
		f 3 -7 -747 747
		mu 0 3 7 6 405
		f 3 -8 -748 748
		mu 0 3 8 7 406
		f 3 -9 -749 749
		mu 0 3 9 8 407
		f 3 -10 -750 750
		mu 0 3 10 9 408
		f 3 -11 -751 751
		mu 0 3 11 10 409
		f 3 -12 -752 752
		mu 0 3 12 11 410
		f 3 -13 -753 753
		mu 0 3 13 12 411
		f 3 -14 -754 754
		mu 0 3 14 13 412
		f 3 -15 -755 755
		mu 0 3 15 14 413
		f 3 -16 -756 756
		mu 0 3 16 15 414
		f 3 -17 -757 757
		mu 0 3 17 16 415
		f 3 -18 -758 758
		mu 0 3 18 17 416
		f 3 -19 -759 759
		mu 0 3 19 18 417
		f 3 -20 -760 740
		mu 0 3 20 19 418
		f 3 360 761 -761
		mu 0 3 378 379 419
		f 3 361 762 -762
		mu 0 3 379 380 420
		f 3 362 763 -763
		mu 0 3 380 381 421
		f 3 363 764 -764
		mu 0 3 381 382 422
		f 3 364 765 -765
		mu 0 3 382 383 423
		f 3 365 766 -766
		mu 0 3 383 384 424
		f 3 366 767 -767
		mu 0 3 384 385 425
		f 3 367 768 -768
		mu 0 3 385 386 426
		f 3 368 769 -769
		mu 0 3 386 387 427
		f 3 369 770 -770
		mu 0 3 387 388 428
		f 3 370 771 -771
		mu 0 3 388 389 429
		f 3 371 772 -772
		mu 0 3 389 390 430
		f 3 372 773 -773
		mu 0 3 390 391 431
		f 3 373 774 -774
		mu 0 3 391 392 432
		f 3 374 775 -775
		mu 0 3 392 393 433
		f 3 375 776 -776
		mu 0 3 393 394 434
		f 3 376 777 -777
		mu 0 3 394 395 435
		f 3 377 778 -778
		mu 0 3 395 396 436
		f 3 378 779 -779
		mu 0 3 396 397 437
		f 3 379 760 -780
		mu 0 3 397 398 438;
	setAttr ".cd" -type "dataPolyComponent" Index_Data Edge 0 ;
	setAttr ".cvd" -type "dataPolyComponent" Index_Data Vertex 0 ;
	setAttr ".pd[0]" -type "dataPolyComponent" Index_Data UV 0 ;
	setAttr ".hfd" -type "dataPolyComponent" Index_Data Face 0 ;
	setAttr ".ndt" 0;
createNode displayLayer -n "skeleton_LYR";
	rename -uid "52CED651-4523-49A9-C903-458BC94E9745";
	setAttr ".dt" 2;
	setAttr ".do" 1;
createNode displayLayerManager -n "layerManager";
	rename -uid "BA139380-4095-8DA2-6AA2-E2AF2FD4AD87";
	setAttr -s 3 ".dli[1:2]"  1 2;
	setAttr -s 3 ".dli";
createNode displayLayer -n "geometry_LYR";
	rename -uid "B8EF96FC-4CB5-9394-50ED-3A8FD6E94C2E";
	setAttr ".do" 2;
createNode groupId -n "groupId17";
	rename -uid "C69A8AF8-471B-AA78-896C-72A80D914161";
	setAttr ".ihi" 0;
createNode objectSet -n "topoSymmetrySet";
	rename -uid "32C2F3F5-45D0-FCE4-2934-ABBAE1C3D083";
	setAttr ".ihi" 0;
createNode skinCluster -n "buddy_body_skin";
	rename -uid "3A06A460-4208-8A13-14FC-989DD0A85122";
	setAttr -s 1689 ".wl";
	setAttr ".wl[0:313].w"
		2 8 0.15659684793268541 18 0.84340315206731464
		2 8 0.15103875299252789 18 0.84896124700747222
		2 8 0.89465858779384921 18 0.10534141220615081
		2 8 0.88530728015124027 18 0.11469271984875971
		2 8 0.89254104253881328 18 0.10745895746118671
		2 8 0.15538399817597404 18 0.84461600182402596
		2 8 0.15550578999662737 18 0.84449421000337266
		2 8 0.89358307767395573 18 0.10641692232604431
		2 8 0.14972101532159426 18 0.85027898467840579
		2 8 0.89596765208168661 18 0.10403234791831344
		2 8 0.89265867658721665 18 0.10734132341278338
		2 8 0.1438817078689196 18 0.85611829213108037
		2 8 0.14412148932403945 18 0.85587851067596055
		2 8 0.89143837845351692 18 0.10856162154648309
		2 8 0.1551440357840573 18 0.84485596421594278
		2 8 0.88497527099734763 18 0.11502472900265238
		2 8 0.87492867746751468 18 0.12507132253248537
		2 8 0.15684986464720782 18 0.84315013535279215
		2 8 0.16037011627515255 18 0.83962988372484748
		2 8 0.86367686076745298 18 0.13632313923254705
		2 8 0.15537307897312561 18 0.84462692102687442
		2 8 0.87127263200177263 18 0.12872736799822743
		2 8 0.87900557260365797 18 0.12099442739634203
		2 8 0.15459854165263043 18 0.84540145834736957
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		2 18 0.84185420317844584 26 0.15814579682155422
		2 18 0.94244062557664554 26 0.057559374423354362
		2 18 0.95274466760218168 26 0.047255332397818268
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		1 18 1
		2 18 0.79057593182491881 26 0.20942406817508125
		3 2 0.012222047059768346 18 0.81333236804592524 26 0.17444558489430642
		3 2 0.080314739263181584 18 0.12622410203236864 26 0.79346115870444989
		3 2 0.012661145984244808 18 0.064082432991439703 26 0.92325642102431549
		3 3 0.023915722171441788 18 0.21586820947034058 26 0.76021606835821764
		3 3 0.045013780374374979 18 0.59399063861109869 26 0.36099558101452645
		3 3 0.058441615125751911 18 0.80497955578591252 26 0.13657882908833563
		3 3 0.080459318620136111 18 0.78300173709186149 26 0.13653894428800248
		3 3 0.078669359915784762 18 0.77597405251379481 26 0.1453565875704205
		3 3 0.043721104526905141 18 0.78416472150420113 26 0.17211417396889378
		3 3 0.01582034076464809 18 0.79573144487001279 26 0.18844821436533904
		2 18 0.61168436437882656 26 0.38831563562117355
		3 2 0.06900624224330805 18 0.3298556761479351 26 0.60113808160875692
		3 2 0.12832406205056293 18 0.11958721740802367 26 0.7520887205414134
		1 20 1
		1 20 1
		2 8 0.81937544255924555 20 0.1806245574407544
		3 8 0.68124269240963586 20 0.15923839619946809 24 0.15951891139089608
		1 20 1
		1 22 1
		3 8 0.83625580344062922 20 0.011380621984151611 22 0.1523635745752191
		3 8 0.84111275563709087 20 0.13244688225497084 22 0.026440362107938348
		1 22 1
		2 8 0.82657589598240822 22 0.17342410401759176
		3 8 0.79458539176493503 20 0.022332263948187301 22 0.18308234428687772
		1 22 1
		1 20 1
		3 8 0.83065532342066417 20 0.11620258785012839 22 0.053142088729207379
		1 20 1
		4 8 0.735174968377456 20 0.19611322018713434 22 0.0105833608760546 
		24 0.058128450559355074
		1 22 1
		1 22 1
		1 22 1
		1 22 1
		2 20 0.016074836024959502 22 0.9839251639750406
		1 22 1
		1 20 1
		1 20 1
		1 22 1
		1 22 1
		1 22 1
		1 22 1
		1 22 1
		1 22 1
		1 22 1
		1 22 1
		1 20 1
		1 12 1
		2 12 0.10865960154119497 24 0.89134039845880508
		2 10 0.1874740813743298 24 0.81252591862567025
		2 12 0.32274228297914315 24 0.67725771702085691
		2 12 0.1059218446559104 24 0.89407815534408963
		1 12 1
		1 12 1
		2 12 0.11345460078261252 24 0.88654539921738751
		1 12 1
		2 12 0.10905730982637096 24 0.89094269017362904
		2 12 0.18904108684741078 24 0.81095891315258928
		1 24 1
		1 24 1
		2 12 0.37559262864850101 24 0.62440737135149893
		2 12 0.1709097234059844 24 0.82909027659401557
		1 24 1
		1 12 1
		1 12 1
		1 12 1
		1 12 1
		1 12 1
		1 12 1
		1 12 1
		1 12 1
		2 12 0.98348481391268405 24 0.016515186087315992
		3 8 0.072452161589845132 10 0.12378299982076113 24 0.80376483858939374
		1 10 1
		2 10 0.10699697515359827 24 0.89300302484640171
		1 24 1
		1 24 1
		1 24 1
		2 8 0.080633358576332886 24 0.91936664142366709
		2 8 0.025570318983214647 24 0.9744296810167854
		1 24 1
		2 16 0.097317626651133152 24 0.90268237334886692
		2 16 0.11694943147789592 24 0.88305056852210406
		1 24 1
		1 24 1
		1 24 1
		1 24 1
		1 24 1
		2 16 0.025336068475312765 24 0.97466393152468733
		1 24 1
		2 16 0.110487095886821 24 0.88951290411317896
		2 16 0.12190761954215142 24 0.87809238045784865
		1 24 1
		1 24 1
		2 16 0.11759402789621173 24 0.88240597210378824
		1 24 1
		1 16 1
		1 16 1
		1 16 1
		1 16 1
		1 16 1
		1 16 1
		1 16 1
		1 16 1
		1 16 1
		1 16 1
		1 16 1
		2 16 0.38987275625740703 24 0.61012724374259297
		1 16 1
		1 16 1
		2 16 0.20706148216163411 24 0.79293851783836589
		2 16 0.90028344928625159 24 0.099716550713748506
		2 16 0.41189989037106073 24 0.58810010962893933
		3 8 0.69212446659405003 20 0.039294393224767861 24 0.26858114018118212
		3 8 0.85524409643357935 20 0.017901004112109729 24 0.12685489945431092
		2 8 0.65031102192258494 24 0.34968897807741506
		1 8 1
		2 8 0.86240274370565817 24 0.13759725629434183
		3 8 0.027663568061738007 10 0.10105223632335113 24 0.87128419561491088
		1 24 1
		1 24 1
		1 24 1
		2 8 0.90614556565765392 24 0.093854434342346096
		1 8 1
		1 24 1
		1 24 1
		2 8 0.014904180422502902 24 0.98509581957749714
		1 24 1
		1 24 1
		2 8 0.011042920559877817 24 0.98895707944012223
		2 8 0.054437921326298419 24 0.94556207867370157
		2 8 0.064234151427739139 24 0.93576584857226086
		2 8 0.11516315768705075 24 0.88483684231294923
		2 8 0.97152312353571635 20 0.028476876464283742
		1 8 1
		2 8 0.98059456973204806 22 0.019405430267951943
		1 8 1
		2 8 0.85989165880525031 24 0.14010834119474977
		2 8 0.77325618033281862 24 0.22674381966718143
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		2 8 0.86721587557630286 24 0.13278412442369716
		1 8 1
		2 8 0.86603210347828985 24 0.13396789652171018
		1 8 1
		2 8 0.86060660281002554 24 0.1393933971899744
		1 8 1
		2 8 0.72593600768509481 24 0.27406399231490519
		1 8 1
		1 24 1
		2 8 0.88140177240427786 24 0.11859822759572215
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		1 8 1
		2 9 0.15103874320313435 19 0.84896125679686563
		2 9 0.8946585759412371 19 0.10534142405876296
		2 9 0.89254104287217584 19 0.10745895712782413
		2 9 0.15538401254415449 19 0.84461598745584554
		2 9 0.89358307789689417 19 0.10641692210310584
		2 9 0.15550580824574411 19 0.84449419175425589
		2 9 0.14972100938619617 19 0.85027899061380385
		2 9 0.8959676553811452 19 0.10403234461885476
		2 9 0.89265866716288511 19 0.10734133283711494
		2 9 0.14388169706071868 19 0.85611830293928137
		2 9 0.89143837888241051 19 0.10856162111758953
		2 9 0.14412149174799496 19 0.85587850825200507
		2 9 0.15514405217960572 19 0.8448559478203943
		2 9 0.88497528644591972 19 0.11502471355408024
		2 9 0.87492867305978228 19 0.12507132694021775
		2 9 0.15684985564558668 19 0.84315014435441338
		2 9 0.86367685414679685 19 0.13632314585320321
		2 9 0.16037011128534046 19 0.83962988871465949
		2 9 0.15537308738261216 19 0.84462691261738787
		2 9 0.87127264245040281 19 0.12872735754959719
		2 9 0.87900557206308672 19 0.12099442793691331
		2 9 0.15459853712120039 19 0.84540146287879969
		2 9 0.88530758147900612 19 0.11469241852099392
		2 9 0.15659661071918968 19 0.84340338928081027
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		1 19 1
		2 19 0.95274466720853712 26 0.047255332791462802
		1 19 1
		3 3 0.045013776690511632 19 0.5939906565232228 26 0.36099556678626554
		3 3 0.023915721757269689 19 0.21586822788528953 26 0.76021605035744089
		1 19 1
		3 3 0.058441616077629939 19 0.80497955260948295 26 0.13657883131288717
		1 19 1
		1 19 1
		3 3 0.078669363883339685 19 0.77597404121542823 26 0.14535659490123196
		3 3 0.080459326667810041 19 0.78300171538733676 26 0.13653895794485318
		1 19 1
		3 3 0.043721104481390827 19 0.78416472172888874 26 0.17211417378972038
		1 19 1
		1 19 1
		2 19 0.61168438587728424 26 0.38831561412271576
		3 3 0.015820340006540388 19 0.79573145465880646 26 0.18844820533465317
		2 19 0.7905759422010914 26 0.20942405779890855
		3 2 0.069006244397028557 19 0.329855676134656 26 0.60113807946831543
		3 2 0.012222047070658047 19 0.81333236787960661 26 0.17444558504973542
		2 19 0.8418541990952888 26 0.1581458009047112
		3 2 0.080314736179161572 19 0.12622410387878424 26 0.79346115994205413
		3 2 0.12832406380156444 19 0.11958721595040939 26 0.75208872024802631
		2 19 0.94244062451975552 26 0.057559375480244437
		3 2 0.012661145361039888 19 0.064082434922329901 26 0.92325641971663019
		1 23 1
		1 21 1
		1 21 1
		1 21 1
		1 23 1
		1 23 1
		1 23 1
		1 23 1
		1 23 1
		1 23 1
		1 21 1
		1 21 1
		1 21 1
		1 23 1
		1 21 1
		1 21 1
		1 23 1
		1 23 1
		1 23 1
		1 23 1
		1 23 1
		1 23 1
		2 21 0.016074833067585732 23 0.98392516693241427
		1 23 1
		1 23 1
		2 9 0.81937545594986871 21 0.18062454405013129
		3 9 0.68124269326681464 21 0.15923840513355067 25 0.15951890159963475
		3 9 0.84111278250562305 21 0.13244685606878048 23 0.026440361425596446
		3 9 0.83625585504991384 21 0.011380618947058108 23 0.15236352600302802
		2 9 0.82657594373607102 23 0.17342405626392898
		3 9 0.79458537780648053 21 0.022332271163576962 23 0.18308235102994255
		3 9 0.83065526489885799 21 0.11620263096981556 23 0.053142104131326397
		4 9 0.73517492060387779 21 0.19611326623462397 23 0.010583364129965389 
		25 0.058128449031532942
		1 11 1
		1 11 0.18747407739751748;
	setAttr ".wl[313:690].w"
		1 25 0.81252592260248258
		1 25 1
		2 11 0.10699696377330324 25 0.89300303622669686
		2 13 0.10592186593445989 25 0.89407813406554015
		2 13 0.10865960132992787 25 0.89134039867007209
		3 9 0.072452153811291553 11 0.1237829976834116 25 0.80376484850529684
		2 9 0.025570308554449681 25 0.97442969144555036
		2 9 0.080633360061351925 25 0.91936663993864809
		2 13 0.11345463090472944 25 0.8865453690952706
		1 25 1
		1 25 1
		2 13 0.10905730497136552 25 0.89094269502863455
		1 25 1
		1 25 1
		1 25 1
		2 13 0.32274234159955334 25 0.6772576584004466
		1 13 1
		1 13 1
		2 13 0.17090977706623636 25 0.82909022293376367
		1 13 1
		1 13 1
		2 13 0.37559271878943795 25 0.62440728121056199
		2 13 0.98348481943037769 25 0.016515180569622343
		1 13 1
		1 13 1
		1 13 1
		1 13 1
		1 13 1
		1 13 1
		1 13 1
		1 13 1
		2 13 0.18904116383968897 25 0.81095883616031106
		1 17 1
		2 17 0.11694947793460983 25 0.88305052206539014
		1 25 1
		2 17 0.38987273046787624 25 0.61012726953212371
		1 25 1
		2 17 0.20706141027392266 25 0.79293858972607734
		2 17 0.02533605454014384 25 0.97466394545985613
		2 17 0.41189972027552463 25 0.58810027972447532
		2 17 0.11048707504824837 25 0.8895129249517516
		1 17 1
		2 17 0.12190766375531369 25 0.8780923362446863
		2 17 0.1175940805175371 25 0.88240591948246294
		1 17 1
		1 17 1
		2 17 0.09731767047965259 25 0.90268232952034744
		1 17 1
		1 17 1
		1 17 1
		1 17 1
		1 17 1
		1 17 1
		2 17 0.90028341046139848 25 0.099716589538601477
		1 17 1
		1 17 1
		1 17 1
		1 25 1
		1 25 1
		1 25 1
		1 25 1
		1 25 1
		1 25 1
		1 25 1
		1 25 1
		3 9 0.69212441941302039 21 0.039294404449224579 25 0.26858117613775501
		3 9 0.85524406049204516 21 0.01790101257801379 25 0.12685492692994108
		2 9 0.65031100914594242 25 0.34968899085405758
		1 9 1
		2 9 0.86240274989259769 25 0.13759725010740229
		3 9 0.027663561881434789 11 0.10105222285155641 25 0.87128421526700883
		1 25 1
		1 25 1
		1 25 1
		2 9 0.9061455698540074 25 0.093854430145992693
		1 9 1
		1 25 1
		1 25 1
		2 9 0.014904180313335812 25 0.98509581968666415
		1 25 1
		1 25 1
		2 9 0.011042922660100684 25 0.98895707733989935
		2 9 0.054437931319019334 25 0.94556206868098069
		2 9 0.11516315591929463 25 0.8848368440807054
		2 9 0.064234163197046168 25 0.93576583680295389
		2 9 0.971523110625654 21 0.028476889374345948
		1 9 1
		2 9 0.9805945652097825 23 0.019405434790217457
		1 9 1
		2 9 0.85989166745488199 25 0.14010833254511804
		2 9 0.77325621396144673 25 0.22674378603855322
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		2 9 0.86721590674559046 25 0.13278409325440951
		1 9 1
		2 9 0.86603207787303593 25 0.13396792212696404
		1 9 1
		2 9 0.86060656733970431 25 0.13939343266029564
		1 9 1
		2 9 0.7259360144232514 25 0.27406398557674866
		1 9 1
		1 25 1
		2 9 0.88140176988694419 25 0.11859823011305581
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 9 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.94977416760932176 26 0.050225832390678259
		1 2 1
		1 2 1
		2 2 0.94977416258540692 26 0.050225837414593112
		2 2 0.90292318656206116 26 0.097076813437938891
		1 2 1
		1 2 1
		2 2 0.90292318438194663 26 0.097076815618053383
		2 2 0.89245740834262977 26 0.10754259165737021
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.90118719930552682 26 0.098812800694473193
		2 2 0.90118718271652232 26 0.098812817283477739
		2 2 0.67443677031805738 26 0.32556322968194273
		2 2 0.6437187767928706 26 0.35628122320712946
		2 2 0.67443676881052061 26 0.32556323118947933
		2 2 0.7290082640404606 26 0.27099173595953951
		2 2 0.72900826297332721 26 0.27099173702667279
		3 2 0.019950999079003578 3 0.043192378548480979 26 0.93685662237251544
		2 2 0.2032147784337377 26 0.7967852215662623
		2 2 0.098241192908194594 26 0.90175880709180534
		2 2 0.098241193345070615 26 0.90175880665492936
		2 2 0.20321478593680778 26 0.79678521406319225
		3 2 0.019950999059101013 3 0.043192380830620693 26 0.93685662011027837
		2 2 0.2205760982085381 26 0.77942390179146193
		2 2 0.20385103109466313 26 0.79614896890533682
		2 2 0.22057609733566563 26 0.77942390266433437
		2 2 0.90111064569548271 26 0.098889354304517355
		2 2 0.90111066324688238 26 0.098889336753117632
		1 2 1
		1 2 1
		2 2 0.64185130668783752 26 0.35814869331216248
		2 2 0.64185129671907537 26 0.35814870328092463
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.99103630134227916 26 0.0089636986577207693
		1 2 1
		1 2 1
		2 2 0.99103629636641855 26 0.0089637036335814615
		2 2 0.99266879195974989 26 0.007331208040250118
		1 2 1
		1 2 1
		2 2 0.99266878835797434 26 0.007331211642025646
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.96892550371404917 26 0.031074496285950898
		2 2 0.96892550302574099 26 0.031074496974259059
		1 2 1
		1 2 1
		2 2 0.65603895953026059 26 0.34396104046973935
		2 2 0.66943119611942681 26 0.33056880388057319
		2 2 0.66943117215026171 26 0.33056882784973823
		2 2 0.65603896173753828 26 0.34396103826246177
		2 2 0.60504143733220883 26 0.39495856266779122
		2 2 0.60504143805858068 26 0.39495856194141932
		2 2 0.7241365951598947 26 0.27586340484010535
		2 2 0.72413660358730692 26 0.27586339641269314
		2 2 0.14372586703903223 26 0.85627413296096777
		2 2 0.20310876286381549 26 0.79689123713618459
		2 2 0.20310876299210795 26 0.79689123700789211
		2 2 0.14372586858113784 26 0.85627413141886211
		2 2 0.031413383630498289 26 0.96858661636950172
		2 2 0.031413384218873457 26 0.96858661578112648
		2 2 0.32496317263010754 26 0.67503682736989246
		2 2 0.32496316197517339 26 0.67503683802482661
		3 2 0.010679580790385399 3 0.035103577395563072 26 0.95421684181405153
		3 2 0.020216647310731115 3 0.023597133532397348 26 0.95618621915687152
		3 2 0.020216646715995958 3 0.023597134366812343 26 0.95618621891719169
		3 2 0.010679580338284445 3 0.035103578232046517 26 0.95421684142966912
		2 3 0.048008308561893663 26 0.95199169143810636
		2 3 0.048008308666799566 26 0.9519916913332005
		3 2 0.045424874159887933 3 0.018017701172939982 26 0.93655742466717207
		3 2 0.045424873555626633 3 0.018017700361493902 26 0.93655742608287951
		2 3 0.12287818186299097 26 0.87712181813700907
		2 3 0.10064354661532322 26 0.89935645338467674
		2 3 0.1006435449940981 26 0.89935645500590189
		2 3 0.12287818061966818 26 0.87712181938033185
		3 3 0.11979298344916173 19 0.15553114392474596 26 0.72467587262609245
		3 3 0.11979298760802333 18 0.15553111403701214 26 0.7246758983549646
		2 3 0.09348977180907693 26 0.90651022819092308
		2 3 0.0934897717237681 26 0.90651022827623196
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.85784136949148282 37 0.14215863050851715
		2 2 0.85784136805203426 38 0.14215863194796582
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1;
	setAttr ".wl[691:1042].w"
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 27 1
		2 27 0.94912944982145464 37 0.050870550178545242
		2 28 0.94912944877141669 38 0.050870551228583354
		1 28 1
		2 27 0.93798674700458506 37 0.062013252995414943
		1 27 1
		1 28 1
		2 28 0.93798674563760731 38 0.062013254362392682
		2 27 0.92805879101869715 37 0.071941208981302879
		1 27 1
		1 28 1
		2 28 0.92805879498221744 38 0.071941205017782631
		2 27 0.92711240391456418 37 0.072887596085435824
		1 27 1
		1 28 1
		2 28 0.92711242607444366 38 0.072887573925556323
		2 27 0.95948273354324565 37 0.040517266456754367
		1 27 1
		1 28 1
		2 28 0.95948273330133227 38 0.040517266698667752
		2 27 0.97085995333957176 37 0.029140046660428194
		1 27 1
		1 28 1
		2 28 0.97085995156490545 38 0.029140048435094577
		2 27 0.96648079060924363 37 0.033519209390756345
		1 27 1
		1 28 1
		2 28 0.96648078120935865 38 0.033519218790641334
		2 27 0.96705558084213272 37 0.032944419157867298
		1 27 1
		1 28 1
		2 28 0.96705557622205673 38 0.032944423777943231
		2 27 0.96947709627628653 37 0.030522903723713572
		1 27 1
		2 28 0.96947709152693429 38 0.030522908473065706
		1 28 1
		2 27 0.94512020673862229 37 0.054879793261377763
		1 27 1
		2 28 0.94512022328881151 38 0.054879776711188481
		1 28 1
		2 27 0.92805868858089857 37 0.071941311419101484
		1 27 1
		2 28 0.92805869216413317 38 0.071941307835866913
		1 28 1
		2 27 0.95836221914069364 37 0.041637780859306348
		1 27 1
		2 28 0.95836221928287946 38 0.041637780717120584
		1 28 1
		2 27 0.077372753005611514 37 0.92262724699438847
		2 27 0.084662338857808758 37 0.91533766114219128
		2 27 0.092077056853748035 37 0.90792294314625199
		2 27 0.10242475699064407 37 0.89757524300935598
		2 28 0.092077054462533434 38 0.90792294553746666
		2 28 0.10242475911676735 38 0.89757524088323259
		2 27 0.11446460555828492 37 0.88553539444171503
		2 28 0.11446460166239501 38 0.88553539833760497
		2 27 0.1151149793980891 37 0.88488502060191088
		2 27 0.11662882873010225 37 0.88337117126989784
		2 28 0.11511498187223396 38 0.88488501812776599
		2 28 0.11662886521091573 38 0.88337113478908424
		2 27 0.10472562827243986 37 0.89527437172756019
		2 28 0.10472565631304696 38 0.89527434368695302
		2 27 0.093662897622839164 37 0.90633710237716081
		2 27 0.081128580787028393 37 0.91887141921297166
		2 28 0.093662898533470623 38 0.90633710146652935
		2 28 0.081128573680920898 38 0.91887142631907914
		2 27 0.079746445397460133 37 0.92025355460253988
		2 28 0.079746436988737693 38 0.92025356301126227
		2 27 0.080363161082995779 37 0.91963683891700421
		2 28 0.080363139269490527 38 0.91963686073050943
		2 28 0.077372738538831171 38 0.9226272614611688
		2 28 0.084662343368534551 38 0.91533765663146549
		2 2 0.75049073178809778 37 0.24950926821190228
		2 2 0.94420956142641577 37 0.055790438573584275
		2 2 0.94075321792433408 37 0.05924678207566593
		2 2 0.9302560696880211 37 0.069743930311978924
		2 2 0.94075321619899832 38 0.059246783801001755
		2 2 0.93025607246523989 38 0.069743927534760108
		3 2 0.87889172919023373 26 0.043329940785635533 37 0.077778330024130676
		3 2 0.87889173476254723 26 0.043329939651252605 38 0.077778325586200209
		3 2 0.76752349493560379 26 0.15063735012544074 37 0.081839154938955577
		3 2 0.57281793034712669 26 0.11732863009998301 37 0.3098534395528903
		3 2 0.7675234991176294 26 0.15063734869861553 38 0.081839152183755037
		3 2 0.57281794789754659 26 0.11732863313300432 38 0.30985341896944901
		3 2 0.30650746328104161 26 0.026937577570863493 37 0.66655495914809493
		3 2 0.30650746983666632 26 0.026937578045572619 38 0.66655495211776117
		1 37 1
		1 37 1
		1 38 1
		1 38 1
		1 37 1
		1 38 1
		2 2 0.40711181088634729 37 0.59288818911365271
		2 2 0.40711180478167247 38 0.59288819521832747
		2 2 0.750490718552559 38 0.24950928144744106
		2 2 0.94420955938917106 38 0.055790440610828995
		2 2 0.050658612060197887 37 0.94934138793980216
		2 2 0.027869304632597496 37 0.97213069536740249
		2 2 0.032967313546682371 37 0.96703268645331764
		2 2 0.054049456255385063 37 0.94595054374461496
		2 2 0.067706009687681479 37 0.93229399031231852
		2 2 0.085979313239810964 37 0.91402068676018899
		3 2 0.10896175174125311 26 0.013899399297286109 37 0.87713884896146088
		1 37 1
		1 37 1
		1 37 1
		1 37 1
		1 37 1
		2 2 0.050658605165849316 38 0.9493413948341507
		2 2 0.027869303132022204 38 0.97213069686797782
		2 2 0.032967312718769799 38 0.96703268728123026
		2 2 0.054049458728544039 38 0.94595054127145595
		2 2 0.067706016029751723 38 0.93229398397024832
		2 2 0.085979321803942174 38 0.91402067819605781
		3 2 0.108961771458663 26 0.013899401812482025 38 0.8771388267288549
		1 38 1
		1 38 1
		1 38 1
		1 38 1
		1 38 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		3 39 0.13867138706398846 41 0.018986053437086571 43 0.84234255949892511
		1 41 1
		1 41 1
		1 39 1
		1 40 1
		1 42 1
		1 42 1
		3 40 0.13867140453732008 42 0.018986047927567706 44 0.84234254753511217
		1 39 1
		1 41 1
		1 42 1
		1 40 1
		1 39 1
		1 41 1
		1 42 1
		1 40 1
		1 41 1
		3 39 0.019905348262580041 41 0.0748564797142732 43 0.90523817202314683
		3 40 0.019905334126854839 42 0.074856497449497877 44 0.90523816842364735
		1 42 1
		2 41 0.042951354961306157 43 0.9570486450386938
		1 41 1
		1 42 1
		2 42 0.04295135634975554 44 0.95704864365024445
		1 41 1
		1 41 1
		1 42 1
		1 42 1
		1 41 1
		1 41 1
		1 42 1
		1 42 1
		1 41 1
		1 41 1
		1 42 1
		1 42 1
		1 39 1
		1 41 1
		1 41 1
		1 41 1
		1 41 1
		1 41 1
		1 39 1
		1 40 1
		1 42 1
		1 42 1
		1 42 1
		1 42 1
		1 42 1
		1 40 1
		1 41 1
		1 41 1
		1 41 1
		1 41 1
		1 41 1
		1 41 1
		1 41 1
		1 41 1
		1 42 1
		1 42 1
		1 42 1
		1 42 1
		1 42 1
		1 42 1
		1 42 1
		1 42 1
		1 29 1
		1 43 1
		2 31 0.41439240571177755 43 0.58560759428822251
		1 31 1
		1 32 1
		2 32 0.41439242115875674 44 0.58560757884124326
		1 44 1
		1 30 1
		1 31 1
		2 29 0.18994093640305032 43 0.81005906359694968
		2 30 0.18994096988573078 44 0.81005903011426916
		1 32 1
		1 43 1
		1 31 1
		1 32 1
		1 44 1
		1 43 1
		1 31 1
		1 32 1
		1 44 1
		1 43 1
		1 31 1
		1 32 1
		1 44 1
		1 31 1
		1 31 1
		1 32 1
		1 32 1
		1 31 1
		1 31 1
		1 32 1
		1 32 1
		1 31 1
		1 31 1
		1 32 1
		1 32 1
		2 29 0.1633640630390594 43 0.83663593696094063
		1 31 1
		1 31 1
		1 31 1
		1 31 1
		1 31 1
		1 43 1
		2 30 0.16336409574219063 44 0.83663590425780943
		1 32 1
		1 32 1
		1 32 1
		1 32 1
		1 32 1
		1 44 1
		1 29 1
		3 29 0.12104098661216503 31 0.09908332411395554 43 0.7798756892738794
		1 43 1
		2 31 0.14635829988333432 43 0.85364170011666574
		2 31 0.11384046629023666 43 0.8861595337097633
		2 31 0.11010583694857948 43 0.88989416305142055
		3 29 0.14249881198223976 31 0.12199539600020889 43 0.73550579201755129
		1 29 1
		1 30 1
		3 30 0.12104101243848996 32 0.099083312814823896 44 0.77987567474668618
		1 44 1
		2 32 0.14635824300287198 44 0.85364175699712797
		2 32 0.11384041642162997 44 0.88615958357836999
		2 32 0.11010584648028159 44 0.88989415351971846
		3 30 0.14249883509093642 32 0.12199538753054946 44 0.73550577737851419
		1 30 1
		1 35 1
		1 35 1
		1 43 1
		2 33 0.033968164891325668 43 0.96603183510867441
		2 34 0.033968172372425741 44 0.96603182762757422
		1 44 1
		1 36 1
		1 36 1
		1 43 1
		1 35 1
		1 36 1
		1 44 1
		1 43 1
		1 35 1
		1 36 1
		1 44 1
		2 33 0.085185723443326367 43 0.91481427655667369
		1 35 1
		1 36 1
		2 34 0.08518577317808522 44 0.91481422682191482
		2 33 0.049585424419261823 43 0.95041457558073816
		1 35 1
		1 36 1
		2 34 0.049585425762945341 44 0.95041457423705467
		1 35 1
		1 35 1
		1 36 1
		1 36 1
		1 35 1
		1 35 1
		1 36 1
		1 36 1
		1 35 1
		1 35 1
		1 36 1
		1 36 1
		2 33 0.10328066040475001 43 0.89671933959524996
		1 35 1
		1 35 1
		1 35 1
		1 35 1
		1 35 1
		1 43 1
		2 34 0.10328074067883217 44 0.89671925932116781
		1 36 1
		1 36 1
		1 36 1
		1 36 1
		1 36 1
		1 44 1
		1 33 1
		3 33 0.21542473961040126 35 0.085826360140661417 43 0.69874890024893743
		2 35 0.10114999498527698 43 0.89885000501472301
		2 35 0.14178336601841673 43 0.85821663398158332
		3 33 0.17342553624391785 35 0.09115692809875843 43 0.73541753565732382
		1 33 1
		1 33 1
		1 33 1
		1 34 1
		3 34 0.21542470090630897 36 0.085826426723575072 44 0.69874887237011596
		2 36 0.10115000212402719 44 0.89884999787597286
		2 36 0.14178322174648461 44 0.85821677825351539
		3 34 0.17342554481381062 36 0.091156884489445533 44 0.73541757069674385
		1 34 1
		1 34 1
		1 34 1
		1 43 1
		2 29 0.031043763188320767 43 0.96895623681167919
		2 30 0.031043774071802283 44 0.96895622592819775
		1 44 1
		1 43 1
		1 44 1
		3 27 0.72767928377331825 39 0.11255612303305357 43 0.15976459319362815
		2 27 0.01564838974517023 43 0.98435161025482976
		2 28 0.015648389441844782 44 0.98435161055815523
		3 28 0.72767927498124374 40 0.11255612440308818 44 0.15976460061566813;
	setAttr ".wl[1043:1318].w"
		2 27 0.92176099110323806 39 0.078239008896761927
		2 28 0.92176097289004144 40 0.078239027109958628
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		2 27 0.86793473998351278 43 0.1320652600164873
		2 28 0.86793475240316931 44 0.13206524759683064
		1 27 1
		1 28 1
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		1 43 1
		1 43 1
		1 44 1
		1 44 1
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		4 27 0.70413884049042419 39 0.16121684761534222 41 0.01408111392026574 
		43 0.1205631979739679
		4 28 0.70413892636411179 40 0.16121674665403346 42 0.014081093762047147 
		44 0.12056323321980748
		2 27 0.90677203095942738 39 0.09322796904057265
		2 28 0.90677207961658135 40 0.093227920383418636
		1 43 1
		1 44 1
		1 43 1
		1 44 1
		2 27 0.87822997184491702 43 0.12177002815508307
		2 28 0.87822993932248061 44 0.12177006067751936
		1 27 1
		1 28 1
		2 27 0.88944374383210212 43 0.11055625616789795
		2 28 0.88944372764308599 44 0.11055627235691405
		1 27 1
		1 28 1
		2 27 0.9196411867687424 43 0.080358813231257556
		2 28 0.91964116959687658 44 0.080358830403123374
		1 43 1
		1 44 1
		2 27 0.87340264215627217 43 0.12659735784372786
		2 28 0.87340264319592242 44 0.1265973568040776
		2 27 0.74084777259950851 43 0.25915222740049149
		2 28 0.74084782728542942 44 0.25915217271457053
		1 27 1
		1 28 1
		2 27 0.85483747183660352 43 0.14516252816339653
		2 28 0.85483746109874537 44 0.14516253890125469
		1 27 1
		1 28 1
		2 27 0.73438523891246277 43 0.26561476108753723
		2 28 0.73438528384113766 44 0.2656147161588624
		1 27 1
		1 28 1
		1 43 1
		1 44 1
		2 27 0.85873276483786942 43 0.14126723516213052
		2 28 0.85873278627050342 44 0.14126721372949655
		1 27 1
		1 28 1
		1 27 1
		1 28 1
		1 27 1
		1 28 1
		1 27 1
		1 28 1
		2 27 0.85719905376576588 39 0.14280094623423414
		2 28 0.85719904963440563 40 0.14280095036559437
		1 27 1
		1 27 1
		1 28 1
		1 28 1
		1 27 1
		1 28 1
		1 27 1
		1 28 1
		1 27 1
		1 28 1
		1 27 1
		1 27 1
		1 27 1
		1 27 1
		1 27 1
		1 27 1
		1 27 1
		1 27 1
		1 27 1
		1 28 1
		1 28 1
		1 28 1
		1 28 1
		1 28 1
		1 28 1
		1 28 1
		1 28 1
		1 28 1
		2 2 0.32827031975866372 26 0.67172968024133628
		2 2 0.51181447908394129 26 0.48818552091605871
		2 2 0.51181448196248314 26 0.48818551803751681
		2 2 0.32827032098008246 26 0.67172967901991754
		3 2 0.68310322366721643 26 0.047933200659491582 37 0.26896357567329188
		3 2 0.68310322938333357 26 0.047933200815814932 38 0.26896356980085157
		1 2 1
		1 2 1
		2 2 0.71318118226018812 37 0.28681881773981194
		2 2 0.71318118790874296 38 0.28681881209125709
		2 2 0.18483120212988224 37 0.81516879787011787
		2 2 0.18483120331989977 38 0.81516879668010023
		1 37 1
		1 38 1
		2 2 0.045660329892304653 37 0.95433967010769538
		2 2 0.045660333851905974 38 0.95433966614809407
		4 2 0.014116624120125754 3 0.016613638481428321 18 0.15731290163614176 
		26 0.81195683576230426
		4 2 0.014116623513747012 3 0.016613638908870586 19 0.15731289498636769 
		26 0.81195684259101475
		3 3 0.07630872687024165 18 0.1448533356577745 26 0.77883793747198393
		3 3 0.076308726655588174 19 0.1448533415702207 26 0.77883793177419125
		3 3 0.17842236511857162 18 0.16353076500914998 26 0.65804686987227845
		3 3 0.17842236528086083 19 0.16353076421591656 26 0.65804687050322264
		3 3 0.31011979753583685 18 0.12433127930061388 26 0.56554892316354932
		3 3 0.31011979523041072 19 0.12433128254848812 26 0.5655489222211012
		3 3 0.31818698423428321 18 0.16157267785592783 26 0.52024033790978885
		3 3 0.31818699233503023 19 0.16157265718152455 26 0.52024035048344519
		3 3 0.28281333677768999 18 0.13476308861058664 26 0.58242357461172334
		3 3 0.28281334110213568 19 0.13476307521192207 26 0.58242358368594227
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		3 2 0.95410412684428869 26 0.012612929017227711 37 0.033282944138483699
		3 2 0.95410412678979406 26 0.012612929247656975 38 0.033282943962549057
		2 2 0.79662609264451911 26 0.20337390735548086
		2 2 0.79662608914503952 26 0.20337391085496051
		2 2 0.21612068375158638 26 0.78387931624841367
		2 2 0.21612067882836902 26 0.78387932117163095
		3 2 0.024680206327577964 3 0.025540514604254706 26 0.94977927906816739
		3 2 0.024680205007537499 3 0.025540514523953558 26 0.94977928046850901
		2 3 0.11721428010155494 26 0.88278571989844501
		2 3 0.11721427955047753 26 0.88278572044952253
		2 3 0.27049476627712393 26 0.72950523372287612
		2 3 0.27049476533025618 26 0.72950523466974382
		2 2 0.98450472042432713 26 0.015495279575672854
		2 2 0.98450472158784252 26 0.015495278412157493
		2 2 0.6747870457320837 26 0.32521295426791635
		2 2 0.6747870471916555 26 0.32521295280834456
		2 2 0.12940947183798679 26 0.87059052816201321
		2 2 0.12940947161411531 26 0.87059052838588469
		2 3 0.040022618313551278 26 0.95997738168644875
		2 3 0.040022617902145202 26 0.95997738209785488
		2 3 0.16014068527581399 26 0.83985931472418607
		2 3 0.16014068500367493 26 0.83985931499632505
		2 3 0.33274223880749776 26 0.66725776119250235
		2 3 0.33274223830706789 26 0.66725776169293216
		1 2 1
		1 2 1
		2 2 0.987226461174325 26 0.012773538825675028
		2 2 0.98722646162407601 26 0.012773538375923942
		2 2 0.60795291012023278 26 0.39204708987976722
		2 2 0.60795291653638495 26 0.39204708346361505
		2 2 0.16574344538716196 26 0.83425655461283799
		2 2 0.16574345182817551 26 0.83425654817182437
		3 2 0.013007674438440774 3 0.017871876626190518 26 0.96912044893536875
		3 2 0.013007674609678308 3 0.017871876610797512 26 0.96912044877952419
		2 3 0.14127584538555993 26 0.85872415461444007
		2 3 0.14127584539360774 26 0.85872415460639229
		2 3 0.44283992951306467 26 0.55716007048693539
		2 3 0.44283992953132184 26 0.5571600704686781
		1 2 1
		1 2 1
		2 2 0.97013794393533359 26 0.029862056064666513
		2 2 0.97013794393533359 26 0.029862056064666513
		2 2 0.61440532302887463 26 0.38559467697112543
		2 2 0.61440532302953754 26 0.38559467697046246
		2 2 0.2232257793499588 26 0.7767742206500412
		2 2 0.2232257793499588 26 0.7767742206500412
		2 2 0.018463705582228953 26 0.98153629441777102
		2 2 0.018463705584464557 26 0.98153629441553547
		2 3 0.10219080012563987 26 0.89780919987436014
		2 3 0.10219080018589653 26 0.89780919981410345
		2 3 0.36724322829126399 26 0.63275677170873612
		2 3 0.36724322824753364 26 0.63275677175246636
		1 2 1
		2 2 0.96779307037775775 26 0.032206929622242308
		2 2 0.60150377572402547 26 0.39849622427597459
		2 2 0.21811514030750884 26 0.78188485969249111
		2 2 0.019204738667276493 26 0.98079526133272354
		2 3 0.099670678281867722 26 0.90032932171813229
		2 3 0.33937509696820434 26 0.66062490303179566
		2 3 0.28669086777759228 26 0.71330913222240766
		2 3 0.20508144312687393 26 0.79491855687312618
		2 3 0.20508144099683739 26 0.79491855900316266
		2 3 0.28669086676715971 26 0.71330913323284029
		2 3 0.22313424647978639 26 0.7768657535202137
		2 3 0.22313424534884344 26 0.77686575465115659
		2 3 0.1870625320791377 26 0.8129374679208623
		2 3 0.1870625319479727 26 0.81293746805202738
		2 3 0.068533705583499971 26 0.93146629441650008
		2 3 0.068533706868673189 26 0.93146629313132678
		1 26 1
		1 26 1
		2 3 0.011817679070611079 26 0.98818232092938896
		2 3 0.38001363836583579 26 0.61998636163416432
		2 3 0.38001363873170713 26 0.61998636126829287
		2 3 0.40196964362771381 26 0.5980303563722863
		2 3 0.40196964483774367 26 0.59803035516225633
		2 3 0.31474150193066042 26 0.68525849806933969
		2 3 0.31474150460138733 26 0.68525849539861272
		2 3 0.23291916325183745 26 0.76708083674816263
		2 3 0.23291916401526794 26 0.76708083598473209
		2 3 0.25868781589976114 26 0.74131218410023891
		2 3 0.38317321127383719 26 0.61682678872616292
		2 3 0.3831732114026134 26 0.6168267885973866
		2 3 0.51866859379336328 26 0.48133140620663678
		2 3 0.5186685945198517 26 0.48133140548014836
		2 3 0.57944998083876931 26 0.42055001916123069
		2 3 0.57944998008852944 26 0.42055001991147062
		2 3 0.53935473454807181 26 0.46064526545192819
		2 3 0.53935473620219365 26 0.4606452637978064
		2 3 0.51504242678263867 26 0.48495757321736138
		2 3 0.51504242807311196 26 0.4849575719268881
		2 3 0.54506565432437193 26 0.45493434567562807
		2 3 0.44961999540901965 26 0.55038000459098035
		2 3 0.44961999647288936 26 0.55038000352711069
		2 3 0.64079816781406018 26 0.35920183218593987
		2 3 0.64079816730347061 26 0.35920183269652944
		3 3 0.59718116432363311 7 0.1543130139952204 26 0.24850582168114652
		3 3 0.59718116223427786 6 0.15431301611296055 26 0.24850582165276161
		2 3 0.72749014225133257 26 0.27250985774866737
		2 3 0.72749014207470963 26 0.27250985792529042
		2 3 0.74032295688358585 26 0.25967704311641421
		2 3 0.74032295721134467 26 0.25967704278865539
		2 3 0.75660429197375167 26 0.24339570802624835
		3 3 0.56537312935087614 6 0.014878232658994846 26 0.41974863799012913
		3 3 0.56537312971783116 7 0.014878232504463185 26 0.41974863777770566
		3 3 0.53224481886049368 7 0.23433474775076896 26 0.23342043338873739
		3 3 0.53224482031776443 6 0.23433474532677764 26 0.23342043435545787
		3 3 0.18457128498596467 7 0.7511681012905933 26 0.064260613723442009
		3 3 0.18457128354035857 6 0.75116810338663897 26 0.064260613073002376
		4 1 0.1783402349258405 3 0.59489455236672417 7 0.074438100953009276 
		26 0.15232711175442609
		4 0 0.17834022551845202 3 0.59489456042930622 6 0.074438100087483097 
		26 0.15232711396475856
		4 1 0.077066379270759711 3 0.61709676095369859 7 0.18125152849220388 
		26 0.12458533128333776
		4 0 0.077066374605029575 3 0.61709676947531911 6 0.18125152291655627 
		26 0.12458533300309502
		2 3 0.84622662228071532 26 0.15377337771928473
		4 3 0.24603759359635591 5 0.065094546635388342 7 0.63750933720801184 
		26 0.051358522560243883
		4 3 0.40477112934029003 6 0.25521943826646365 7 0.25521943893795046 
		26 0.084789993455295976
		4 3 0.24603759575541759 4 0.065094542757524868 6 0.63750933847612556 
		26 0.051358523010931995
		4 3 0.46944523322480058 6 0.178928019451841 7 0.17892801789085144 
		26 0.17269872943250691
		3 3 0.18111254854477146 6 0.75655618220239362 26 0.062331269252834852
		3 3 0.18111254944676466 7 0.75655618097855815 26 0.062331269574677099
		2 3 0.64824004669233048 26 0.35175995330766957
		2 3 0.68740894343820891 26 0.31259105656179109
		2 3 0.68740894298342903 26 0.31259105701657108
		2 3 0.53857376697047932 26 0.46142623302952063
		2 3 0.59993497059594303 26 0.40006502940405692
		2 3 0.59993497028376153 26 0.40006502971623842
		2 3 0.66039188078287103 26 0.33960811921712897
		2 3 0.66039188063641385 26 0.33960811936358626
		2 3 0.70741800605933258 26 0.29258199394066742
		1 3 0.70741800676655631;
	setAttr ".wl[1318:1688].w"
		1 26 0.29258199323344375
		3 3 0.59501867812828158 6 0.036054012010421334 26 0.36892730986129713
		3 3 0.58045683603139631 6 0.14955706717327424 26 0.26998609679532942
		3 3 0.58045684076321724 7 0.14955706073404545 26 0.26998609850273719
		3 3 0.59501868006360559 7 0.036054011244931183 26 0.36892730869146323
		3 3 0.56902976869525868 7 0.23038795332557591 26 0.20058227797916547
		3 3 0.56902976654114135 6 0.23038795647929827 26 0.2005822769795603
		4 3 0.10816291999636679 5 0.18093847169272087 7 0.66370698816834006 
		26 0.047191620142572208
		4 3 0.18120386157573959 5 0.24664085499203722 7 0.5108042865903254 
		26 0.06135099684189798
		4 3 0.18120385700201322 4 0.24664086423000511 6 0.51080428347463058 
		26 0.06135099529335112
		4 3 0.10816291138296043 4 0.18093848484105249 6 0.66370698739145506 
		26 0.047191616384532016
		2 5 0.1310054090470214 7 0.8689945909529786
		2 4 0.13100541208719213 6 0.86899458791280781
		4 3 0.01731592411546648 5 0.10856123239499267 7 0.86358654328281115 
		26 0.010536300206729852
		2 3 0.4340357930146661 26 0.5659642069853339
		2 3 0.43403579658068508 26 0.56596420341931486
		4 3 0.017315924161671409 4 0.1085612295475637 6 0.86358654605592056 
		26 0.010536300234844377
		2 5 0.11391515547981083 7 0.8860848445201891
		2 4 0.11391515451213413 6 0.88608484548786592
		2 5 0.11303832196562802 7 0.88696167803437198
		2 4 0.11303832166286082 6 0.88696167833713913
		4 3 0.13550182441404565 5 0.16394815871970364 7 0.64965365293478683 
		26 0.050896363931464024
		4 3 0.1355018254883556 4 0.16394815428838097 6 0.64965365588827373 
		26 0.050896364334989737
		2 1 0.25975236376947158 7 0.74024763623052847
		2 0 0.25975235375940969 6 0.74024764624059025
		2 5 0.28283334719221193 7 0.71716665280778802
		2 4 0.28283335201950249 6 0.71716664798049756
		3 3 0.021124777265869411 5 0.32011002580071257 7 0.65876519693341806
		3 3 0.021124777789191307 4 0.3201100153085496 6 0.65876520690225915
		4 1 0.30058120753926304 3 0.34627206152037404 7 0.28458510761023292 
		26 0.06856162333013012
		4 0 0.30058119748744755 3 0.34627207156585721 6 0.28458510562756667 
		26 0.068561625319128669
		1 1 1
		1 0 1
		3 3 0.54293687870858887 6 0.075477142643492992 26 0.38158597864791816
		3 3 0.5508731130898249 6 0.056287910094765747 26 0.39283897681540941
		3 3 0.54293687811198943 7 0.075477140706139145 26 0.38158598118187137
		3 3 0.55087311317313348 7 0.05628790772115546 26 0.39283897910571119
		2 3 0.46217374156190338 26 0.53782625843809662
		2 3 0.46217374264406302 26 0.53782625735593692
		2 5 0.74370576227632235 7 0.2562942377236776
		2 5 0.78751279263558427 7 0.21248720736441568
		2 4 0.78751280655707012 6 0.21248719344292982
		2 4 0.7437057695512328 6 0.25629423044876726
		2 5 0.84687193086326473 7 0.15312806913673527
		2 4 0.84687193486019752 6 0.15312806513980246
		2 5 0.86222927130861238 7 0.13777072869138765
		2 5 0.85720560050939154 7 0.14279439949060849
		2 4 0.85720559676285202 6 0.14279440323714793
		2 4 0.86222926700916147 6 0.1377707329908385
		3 1 0.078827011171581418 5 0.76654618698885035 7 0.15462680183956831
		3 1 0.23457334493057053 5 0.53703570220521013 7 0.22839095286421934
		3 0 0.23457334158687992 4 0.53703569978335752 6 0.22839095862976258
		3 0 0.078827003486294595 4 0.7665461940667837 6 0.15462680244692173
		1 1 1
		1 0 1
		2 5 0.7107135223669977 7 0.28928647763300236
		2 4 0.71071352499046248 6 0.28928647500953752
		3 1 0.011033702569995023 5 0.60584669759856902 7 0.38311959983143606
		3 0 0.011033702227701664 4 0.60584668932146346 6 0.38311960845083498
		2 1 0.74040464832365349 7 0.25959535167634651
		2 0 0.74040464273361117 6 0.25959535726638888
		1 1 1
		1 0 1
		2 3 0.8216860055183236 26 0.1783139944816764
		2 3 0.83717828764581892 26 0.16282171235418103
		2 3 0.82168600522140023 26 0.17831399477859983
		2 3 0.70355424562500657 26 0.29644575437499349
		3 3 0.60702638520941576 6 0.16893935649357489 26 0.2240342582970094
		3 3 0.60702638520976326 7 0.16893935656439912 26 0.2240342582258377
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 1 1
		1 1 1
		1 1 1
		1 1 1
		1 0 1
		1 0 1
		1 1 1
		1 1 1
		1 0 1
		1 0 1
		1 1 1
		1 1 1
		1 0 1
		1 1 1
		1 0 1
		1 0 1
		1 1 1
		1 1 1
		1 0 1
		1 0 1
		1 1 1
		1 1 1
		1 0 1
		1 1 1
		1 0 1
		1 0 1
		1 1 1
		1 1 1
		1 0 1
		1 1 1
		1 0 1
		2 0 0.17568926750511171 4 0.82431073249488829
		2 1 0.17568929515002188 5 0.82431070484997815
		1 1 1
		3 0 0.20357733977464071 4 0.70588247990325181 6 0.090540180322107477
		3 1 0.20357733313832485 5 0.70588248757645333 7 0.090540179285221831
		1 0 1
		1 1 1
		1 0 1
		1 1 1
		2 0 0.14396887216335355 4 0.85603112783664648
		2 1 0.14396890298218373 5 0.85603109701781632
		1 4 1
		1 4 1
		1 5 1
		1 5 1
		1 4 1
		1 4 1
		1 5 1
		1 5 1
		1 4 1
		1 5 1
		1 4 1
		1 5 1
		2 4 0.98372002808148562 6 0.016279971918514324
		2 5 0.98372002756146193 7 0.016279972438538039
		1 4 1
		1 5 1
		2 4 0.94318079406652888 6 0.056819205933471076
		2 5 0.9431807939952328 7 0.05681920600476726
		2 0 0.12508537534312009 4 0.87491462465687997
		2 1 0.12508537612381629 5 0.87491462387618368
		1 4 1
		1 4 1
		1 5 1
		1 5 1
		1 4 1
		1 5 1
		1 0 1
		1 0 1
		1 1 1
		1 1 1
		1 0 1
		1 1 1
		1 0 1
		2 0 0.35257051004088458 4 0.64742948995911542
		2 1 0.35257050436299048 5 0.64742949563700947
		1 1 1
		2 0 0.14342825523645131 4 0.85657174476354869
		2 1 0.14342826394087549 5 0.85657173605912451
		1 4 1
		1 4 1
		2 0 0.19928168033929261 4 0.80071831966070739
		1 5 1
		1 5 1
		2 1 0.19928168069265983 5 0.80071831930734017
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.98843756591504772 26 0.011562434084952259
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.98331129035669163 26 0.016688709643308465
		2 2 0.97946686868825883 26 0.020533131311741226
		2 2 0.98331129165270736 26 0.016688708347292674
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1;
	setAttr -s 45 ".pm";
	setAttr ".pm[0]" -type "matrix" -4.9445965714596328e-16 -2.3592239273284553e-15 -1 0
		 3.655141400502731e-15 0.99999999999999978 -2.3592239273284592e-15 0 1 -3.5982221934442676e-15 -5.021409014621135e-16 0
		 -5.7322045408835608e-15 -1.2999999999999792 8.5000000000000053 1;
	setAttr ".pm[1]" -type "matrix" -9.1983392873923308e-15 -9.1732177409653244e-15 -0.99999999999999978 0
		 -2.9400723240728159e-15 -0.99999999999999978 9.1593399531575635e-15 0 -0.99999999999999978 2.9456351175170989e-15 9.1874070015623072e-15 0
		 -6.9077569921540175e-14 1.299999999999921 -8.5000000000000107 1;
	setAttr ".pm[2]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -28 -1 0 1;
	setAttr ".pm[3]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -11 -1 0 1;
	setAttr ".pm[4]" -type "matrix" 0.080815868766054871 -2.6547615831701794e-16 -0.99672904811467566 0
		 -0.99672904811467555 7.6856153647732499e-16 -0.080815868766054871 0 7.4045367094602878e-16 1 -2.0276607625338074e-16 0
		 4.3209551166917279 -1.6659031841870784e-15 8.5772575383706133 1;
	setAttr ".pm[5]" -type "matrix" 0.080815868766054788 -8.9277224251399205e-15 -0.99672904811467566 0
		 0.99672904811467589 3.9059674980265064e-16 0.080815868766054774 0 -3.4781929143902126e-16 -0.99999999999999978 8.9262402681233043e-15 0
		 -4.3209551166917297 -7.5049285332698075e-14 -8.5772575383706133 1;
	setAttr ".pm[6]" -type "matrix" 0.098742859722262016 -0.012281602211417567 -0.99503719020998915 0
		 -0.98742859722262477 0.12281602211417433 -0.099503719020998471 0 0.12342857465282848 0.99235345868252578 -1.8908485888147192e-16 0
		 8.1783773564963926 -0.51337097243724905 8.6568235548269019 1;
	setAttr ".pm[7]" -type "matrix" 0.098742859722262349 -0.012281602211426407 -0.99503719020998882 0
		 0.98742859722262477 -0.12281602211417386 0.099503719020999887 0 -0.1234285746528289 -0.99235345868252545 8.9164786665207837e-15 0
		 -8.1783773564963926 0.51337097243717611 -8.6568235548269072 1;
	setAttr ".pm[8]" -type "matrix" 0.99973690272738092 0.022934649941631899 -0.00035659096746199154 0
		 -0.022934651399784185 0.99973696628921893 2.0736088220856333e-14 0 0.00035649717201706059 8.1782895103967676e-06 0.99999993642143903 0
		 -25.148314410141758 -19.302845123270217 1.6771232738530868 1;
	setAttr ".pm[9]" -type "matrix" 0.99973690272738114 0.022934649941631972 -0.00035659096746026039 0
		 0.022934651399784057 -0.99973696628921904 -1.0346657883762716e-13 0 -0.00035649717201719579 -8.178289427662096e-06 -0.99999993642143936 0
		 25.148314410141765 19.302845123270359 -1.6771232738514947 1;
	setAttr ".pm[10]" -type "matrix" 0.96687323370463074 -0.0064362448361374698 0.25517587013265941 0
		 0.00665661434564569 0.99997784449729377 -6.6394372638667336e-15 0 -0.25517021658297795 0.0016986073577941775 0.9668946557417929 0
		 -39.010218020314554 -17.973722239951002 -8.5538951272865571 1;
	setAttr ".pm[11]" -type "matrix" 0.96687323370463096 -0.006436244836136037 0.25517587013265902 0
		 -0.0066566143456458903 -0.99997784449729354 1.1921670248216114e-14 0 0.25517021658297773 -0.0016986073577993416 -0.96689465574179312 0
		 39.010218020314561 17.973722239951041 8.5538951272864363 1;
	setAttr ".pm[12]" -type "matrix" 0.92663734741845671 -0.069700799886994264 0.36943879718351202 0
		 0.075007177835449343 0.99718299387482645 -6.3213323464594882e-15 0 -0.36839808582896871 0.027710561559664108 0.92925506462736074 0
		 -40.628326878558063 -15.240516488963546 -13.378538417879168 1;
	setAttr ".pm[13]" -type "matrix" 0.92663734741845682 -0.069700799887000564 0.36943879718351041 0
		 -0.075007177835449523 -0.99718299387482601 -1.0907941216942168e-14 0 0.36839808582896849 -0.027710561559648118 -0.92925506462736129 0
		 40.628326878558077 15.240516488963305 13.37853841787941 1;
	setAttr ".pm[14]" -type "matrix" 0.95236204559806781 0.17211128701015596 0.25176226680734287 0
		 -0.17783966080066863 0.98405947739265409 -2.081668171172169e-16 0 -0.24774904470162371 -0.044773316131425024 0.96778910978168609 0
		 -35.554435969599758 -21.188748558206171 -8.5496602517730622 1;
	setAttr ".pm[15]" -type "matrix" 0.95236204559806792 0.17211128701015616 0.25176226680734265 0
		 0.17783966080066815 -0.98405947739265431 1.8457457784393235e-15 0 0.24774904470162393 0.044773316131423255 -0.96778910978168631 0
		 35.554435969599758 21.188748558206171 8.5496602517729379 1;
	setAttr ".pm[16]" -type "matrix" 0.96063681300616799 0.23201230989430555 0.15279791083277747 0
		 -0.23476909644539859 0.97205116704534211 -2.0816681711721691e-17 0 -0.14852738754709141 -0.035872227464955678 0.98825745554745936 0
		 -36.528353759534973 -23.469678737291968 -4.4324184805332845 1;
	setAttr ".pm[17]" -type "matrix" 0.9606368130061681 0.23201230989430446 0.15279791083277891 0
		 0.23476909644539867 -0.97205116704534189 -8.8193341518660888e-15 0 0.14852738754709088 0.035872227464964469 -0.98825745554745925 0
		 36.52835375953498 23.469678737291936 4.4324184805333875 1;
	setAttr ".pm[18]" -type "matrix" 0.99368106829059399 0.084974522483502749 0.073329837375820633 0
		 -0.085203913285456678 0.99636353464026595 1.7347234759768066e-16 0 -0.073063175962368526 -0.0062479891050061598 0.99730774335228911 0
		 -14.807324493690764 -20.837416535567982 -0.21263457334875474 1;
	setAttr ".pm[19]" -type "matrix" 0.99368106829059433 0.08497452248350261 0.073329837375820647 0
		 0.085203913285456304 -0.99636353464026617 4.657732532997728e-16 0 0.073063175962368609 0.006247989105005485 -0.99730774335228944 0
		 14.807324493690775 20.837416535567989 0.2126345733487422 1;
	setAttr ".pm[20]" -type "matrix" 0.47542194344605976 -0.11293657638623206 0.87247882805494381 0
		 0.84885696101420938 -0.2016461384253353 -0.48865191557577264 0 0.23111866092484354 0.97292556990363188 -1.1102230246251565e-16 0
		 -35.314075940043388 9.0682599121482781 -20.458691470047007 1;
	setAttr ".pm[21]" -type "matrix" 0.47542194344605937 -0.1129365763862317 0.87247882805494448 0
		 -0.84885696101420927 0.20164613842533655 0.48865191557577164 0 -0.23111866092484443 -0.97292556990363188 8.1878948066105295e-16 0
		 35.314075940043374 -9.0682599121482212 20.45869147004705 1;
	setAttr ".pm[22]" -type "matrix" 0.21522222383529763 -0.011570577349480157 0.97649655202012264 0
		 0.97508844268237926 -0.052421799420097268 -0.2155330227431782 0 0.053683547895432872 0.99855799865874495 -6.5052130349130236e-16 0
		 -31.335040543496159 1.7496992184419178 -30.849835360702269 1;
	setAttr ".pm[23]" -type "matrix" 0.21522222383529768 -0.011570577349479749 0.97649655202012309 0
		 -0.97508844268237926 0.052421799420098239 0.2155330227431779 0 -0.053683547895433649 -0.99855799865874528 1.1726730697603216e-15 0
		 31.335040543496135 -1.7496992184418749 30.849835360702286 1;
	setAttr ".pm[24]" -type "matrix" 0.93616767052418659 0.35114534289864113 -0.01694227924199633 0
		 -0.35119575027525313 0.93630205862670335 -7.6527326142716845e-15 0 0.015863090932106909 0.0059500564697729796 0.99985646928651029 0
		 -25.294461554979442 -29.276137066081382 2.2407646310315723 1;
	setAttr ".pm[25]" -type "matrix" 0.93616767052418692 0.35114534289864363 -0.016942279241942765 0
		 0.35119575027525263 -0.93630205862670346 -1.349051079180264e-13 0 -0.015863090932106784 -0.0059500564696207073 -0.9998564692865114 0
		 25.294461554979474 29.276137066081731 -2.2407646310271123 1;
	setAttr ".pm[26]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -19 -1 0 1;
	setAttr ".pm[27]" -type "matrix" 0.99973979339155139 -0.020401790803298011 -0.010203550432900537 0
		 0.020402852926359051 0.99979184013096711 -7.6992719925256248e-15 0 0.010201426463179043 -0.00020818153880135116 0.9999479424242862 0
		 -27.148036103812238 -34.253232760579778 5.6701129755632778 1;
	setAttr ".pm[28]" -type "matrix" 0.99973979339155117 -0.020401790803299479 -0.010203550432897578 0
		 -0.020402852926358871 -0.99979184013096689 1.4947258383281307e-13 0 -0.010201426463179014 0.0002081815386597814 -0.99994794242428597 0
		 27.148036103812224 34.253232760578982 -5.6701129755681317 1;
	setAttr ".pm[29]" -type "matrix" 0.99530734153820533 0.08646677643655834 0.043437224276190865 0
		 -0.086548464481594617 0.99624764155097389 -1.075397149802315e-12 0 -0.043274232240768741 -0.0037594250613764936 0.99905615835506445 0
		 -40.617194381206382 -40.868709853961192 3.6921640635311892 1;
	setAttr ".pm[30]" -type "matrix" 0.99530734153820521 0.086466776436365869 0.043437224276573892 0
		 0.086548464481594742 -0.99624764155097401 -3.3370112229036392e-12 0 0.043274232240768755 0.0037594250658015767 -0.99905615835504769 0
		 40.61719438120636 40.868709853977556 -3.6921640633501926 1;
	setAttr ".pm[31]" -type "matrix" 0.9979402655783316 0.045314133196497637 -0.045407660918601073 0
		 -0.045360921162700261 0.9989706636489738 5.1230721054285048e-15 0 0.045360921162602853 0.0020597333271062924 0.99896854021030179 0
		 -44.068134909514349 -39.039155615406038 7.7874138475459711 1;
	setAttr ".pm[32]" -type "matrix" 0.9979402655783316 0.045314133196495188 -0.045407660918603564 0
		 0.045360921162700518 -0.99897066364897391 4.9820390868315899e-14 0 -0.045360921162602874 -0.0020597333271610203 -0.99896854021030179 0
		 44.068134909514342 39.039155615405647 -7.7874138475481889 1;
	setAttr ".pm[33]" -type "matrix" 0.99530734153820521 0.086466776436559437 0.043437224276190907 0
		 -0.086548464481595699 0.99624764155097389 -1.0753993182066599e-12 0 -0.043274232240768769 -0.0037594250613765413 0.99905615835506456 0
		 -40.980697932029045 -36.684469759447154 3.6921640635267039 1;
	setAttr ".pm[34]" -type "matrix" 0.9953073415382051 0.086466776436329065 0.043437224276649249 0
		 0.086548464481595797 -0.99624764155097378 -4.2048569487418403e-12 0 0.04327423224076879 0.0037594250666719178 -0.99905615835504447 0
		 40.980697932029031 36.684469759466722 -3.6921640633322506 1;
	setAttr ".pm[35]" -type "matrix" 0.99437882000179478 -0.047138076826326376 -0.094809092628046168 0
		 0.04735137238093217 0.99887829465538125 1.6278645098566358e-14 0 0.094702744762126229 -0.0044893406501449974 0.99549547259394744 0
		 -46.854182971033204 -30.615731964708665 10.045023363937858 1;
	setAttr ".pm[36]" -type "matrix" 0.99437882000179478 -0.047138076826348421 -0.094809092628035191 0
		 -0.047351372380931955 -0.99887829465538114 2.1597133803563651e-13 0 -0.094702744762126173 0.0044893406499137293 -0.99549547259394855 0
		 46.85418297103319 30.615731964706342 -10.045023363945013 1;
	setAttr ".pm[37]" -type "matrix" 0.99616727256193882 0.066302989947811045 0.057049790538513126 0
		 -0.066411151504129767 0.99779234260235494 2.2941717969793269e-16 0 -0.056923844146396542 -0.0037887422827321735 0.99837133442397663 0
		 -13.854314934497115 -36.500661964505689 3.8793857566188681 1;
	setAttr ".pm[38]" -type "matrix" 0.99616727256193882 0.066302989947810836 0.057049790538513376 0
		 0.066411151504129851 -0.99779234260235494 -1.5452049362263405e-15 0 0.05692384414639666 0.00378874228273373 -0.99837133442397663 0
		 13.854314934497113 36.500661964505696 -3.8793857566188183 1;
	setAttr ".pm[39]" -type "matrix" 0.48574139257513332 -0.15477007687694017 0.86029153363419564 0
		 0.81968859997061561 -0.26117450472986165 -0.50980239030169672 0 0.30358837035924591 0.95280328577446549 -1.1102230246251568e-16 0
		 -48.76539993084377 20.365807491048994 -13.321773711574373 1;
	setAttr ".pm[40]" -type "matrix" 0.4857413925751331 -0.15477007687694019 0.86029153363419564 0
		 -0.81968859997061538 0.2611745047298617 0.50980239030169694 0 -0.30358837035924585 -0.95280328577446527 -8.3266726846886704e-17 0
		 48.765399930843763 -20.365807491049026 13.321773711574361 1;
	setAttr ".pm[41]" -type "matrix" 0.36735917918532551 -0.054573509608920705 0.9284766908852633 0
		 0.91839794796334273 -0.13643377402230694 -0.37139067635409329 0 0.14694367167389333 0.9891448616633437 -3.0531133177191805e-16 0
		 -51.966629487558329 11.359476025103717 -21.744924100533268 1;
	setAttr ".pm[42]" -type "matrix" 0.36735917918532535 -0.054573509608920788 0.92847669088526319 0
		 -0.91839794796334262 0.13643377402230716 0.37139067635409351 0 -0.14694367167389336 -0.98914486166334337 6.2450045135165031e-17 0
		 51.966629487558194 -11.35947602510303 21.744924100533339 1;
	setAttr ".pm[43]" -type "matrix" 0.99911229454319339 -0.013310852834388294 0.03996803834886755 0
		 0.013321497260576886 0.99991126491841087 2.5778728110492644e-13 0 -0.039964491781727007 0.00053243411311762186 0.99920095872178971 0
		 -36.945840502481261 -34.51088841345657 3.8449252891525707 1;
	setAttr ".pm[44]" -type "matrix" 0.99911229454319328 -0.01331085283435644 0.039968038348878215 0
		 -0.013321497260576709 -0.99991126491841076 5.398250206220533e-13 0 0.039964491781727035 -0.00053243411391443315 -0.99920095872178927 0
		 36.945840502481246 34.510888413453493 -3.8449252891800985 1;
	setAttr ".gm" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
	setAttr -s 45 ".ma";
	setAttr -s 45 ".dpf[0:44]"  4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 
		4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4;
	setAttr -s 45 ".lw";
	setAttr -s 45 ".lw";
	setAttr ".mmi" yes;
	setAttr ".mi" 4;
	setAttr ".ucm" yes;
	setAttr -s 45 ".ifcl";
	setAttr -s 45 ".ifcl";
createNode dagPose -n "bindPose1";
	rename -uid "338456D1-47E4-9BCE-E906-B38A9E10AA3B";
	setAttr -s 46 ".wm";
	setAttr -s 46 ".xm";
	setAttr ".xm[0]" -type "matrix" "xform" 1 1 1 0 0 0 0 0 11 1 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0.50000000000000011 0.50000000000000011 0.50000000000000011 0.50000000000000011 1
		 1 1 yes;
	setAttr ".xm[1]" -type "matrix" "xform" 1 1 1 0 0 0 0 -2 -1.5 7.7999999999999998 0
		 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.061755838397755176 -0.99684743643587959 0.0030801107979294452 0.049718385055006455 1
		 1 1 yes;
	setAttr ".xm[2]" -type "matrix" "xform" 1 1 1 0 0 0 0 4.0509258201058191 -1.4432899320127035e-15
		 -1.7763568394002505e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.00058012206703673356 -0.0093641866153739376 -0.061829880455362869 0.99804260497751063 1
		 1 1 yes;
	setAttr ".xm[3]" -type "matrix" "xform" 1 1 1 0 0 0 0 3.7121422386541179 2.2601072735094927e-15
		 -1.7763568394002505e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.028596118116467995 -0.028596118116468078 0.70652831650873726 0.70652831650873527 1
		 1 1 yes;
	setAttr ".xm[4]" -type "matrix" "xform" 1 1 1 0 0 0 0 -2 -1.5 -7.7999999999999998 0
		 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.99684743643587947 0.061755838397755163 -0.049718385055006885 0.0030801107979338523 1
		 1 1 yes;
	setAttr ".xm[5]" -type "matrix" "xform" 1 1 1 0 0 0 0 -4.0509258201058174 3.219646771412954e-15
		 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.00058012206703678408 -0.0093641866153747009 -0.061829880455363209 0.99804260497751052 1
		 1 1 yes;
	setAttr ".xm[6]" -type "matrix" "xform" 1 1 1 0 0 0 0 -3.712142238654117 -3.9420889442652884e-15
		 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.028596118116465553 -0.028596118116465646 0.70652831650873749 0.70652831650873515 1
		 1 1 yes;
	setAttr ".xm[7]" -type "matrix" "xform" 1 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[8]" -type "matrix" "xform" 1 1 1 0 0 0 0 8 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[9]" -type "matrix" "xform" 1 1 1 0 0 0 0 9 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[10]" -type "matrix" "xform" 1 1 1 0 0 0 0 0.5 -2 16.5 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.46035699148696779 -0.5014045489434229 -0.53962527270465266 0.49544877005230253 1
		 1 1 yes;
	setAttr ".xm[11]" -type "matrix" "xform" 1 1 1 0 0 0 0 9.1427725554122787 3.5527136788005009e-15
		 1.8596235662471372e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0019943850057409971 -0.036813813098166046 0.031159088442882333 0.99883425892438771 1
		 1 1 yes;
	setAttr ".xm[12]" -type "matrix" "xform" 1 1 1 0 0 0 0 0.5 -2 -16.5 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.50140454894342301 -0.46035699148696774 0.49544877005230281 0.53962527270465244 1
		 1 1 yes;
	setAttr ".xm[13]" -type "matrix" "xform" 1 1 1 0 0 0 0 -9.1427725554122841 -3.5527136788005009e-15
		 -1.6930901125533637e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0019943850056993442 -0.036813813098167426 0.031159088442880692 0.99883425892438782 1
		 1 1 yes;
	setAttr ".xm[14]" -type "matrix" "xform" 1 1 1 0 0 0 0 8.4152140198571352 -7.1054273576010019e-15
		 2.2204460492503131e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0015735019965656776 -0.0081425119623192103 -0.1671613358133755 0.98589467561169952 1
		 1 1 yes;
	setAttr ".xm[15]" -type "matrix" "xform" 1 1 1 0 0 0 0 5.5213954127458642 1.7559403927372088
		 -0.086046692341986919 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.024009999287783388 0.13493410253531787 0.18002177100945094 0.97406800063948806 1
		 1 1 yes;
	setAttr ".xm[16]" -type "matrix" "xform" 1 1 1 0 0 0 0 -8.4152140198571317 1.0658141036401503e-14
		 -1.1102230246251565e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0015735019965313877 -0.0081425119622994379 -0.1671613358133763 0.98589467561169974 1
		 1 1 yes;
	setAttr ".xm[17]" -type "matrix" "xform" 1 1 1 0 0 0 0 -5.5213954127458429 -1.7559403927372088
		 0.086046692341724462 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.024009999287706568 0.13493410253530438 0.18002177100946071 0.97406800063948995 1
		 1 1 yes;
	setAttr ".xm[18]" -type "matrix" "xform" 1 1 1 0 0 0 0 1.8027182253474834 -7.1054273576010019e-15
		 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0024575329621468098 0.060101159399070583 0.034141906083817369 0.99760520318390045 1
		 1 1 yes;
	setAttr ".xm[19]" -type "matrix" "xform" 1 1 1 0 0 0 0 -1.8027182253475047 7.1054273576010019e-15
		 -3.5527136788005009e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0024575329621355706 0.060101159399070389 0.034141906083817723 0.99760520318390045 1
		 1 1 yes;
	setAttr ".xm[20]" -type "matrix" "xform" 1 1 1 0 0 0 0 6.708251434158619 -1.7559403927371946
		 0.086046692342074849 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.03593626956323271 0.13044614476164848 0.08908024874042271 0.98679131387089458 1
		 1 1 yes;
	setAttr ".xm[21]" -type "matrix" "xform" 1 1 1 0 0 0 0 -6.708251434158587 1.7559403927372266
		 -0.086046692341725795 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.035936269563156674 0.13044614476164174 0.089080248740432494 0.98679131387089714 1
		 1 1 yes;
	setAttr ".xm[22]" -type "matrix" "xform" 1 1 1 0 0 0 0 1.6306823725054471 0
		 -3.5527136788005009e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.010428858424365669 -0.049441512364878884 -0.029053902867219331 0.99829987804027098 1
		 1 1 yes;
	setAttr ".xm[23]" -type "matrix" "xform" 1 1 1 0 0 0 0 -1.6306823725054542 -3.5527136788005009e-15
		 -7.1054273576010019e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.010428858424360367 -0.049441512364879134 -0.02905390286721983 0.99829987804027098 1
		 1 1 yes;
	setAttr ".xm[24]" -type "matrix" "xform" 1 1 1 0 0 0 0 0.42272365381576904 3.6617016049174929
		 0.97564700022563899 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.58908352921404739 0.4004640877501241 0.51411626919323483 0.47780076578215419 1
		 1 1 yes;
	setAttr ".xm[25]" -type "matrix" "xform" 1 1 1 0 0 0 0 -0.42272365381576904
		 -3.6617016049173188 -0.9756470002262656 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.58908352921408402 0.4004640877500848 0.51411626919326536 0.47780076578210878 1
		 1 1 yes;
	setAttr ".xm[26]" -type "matrix" "xform" 1 1 1 0 0 0 0 2.5787619122361818 1.7763568394002505e-15
		 -3.5527136788005009e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.020892901461180412 0.14462672919893813 -0.08867628937642881 0.98528326463620508 1
		 1 1 yes;
	setAttr ".xm[27]" -type "matrix" "xform" 1 1 1 0 0 0 0 -2.5787619122361605 1.4210854715202004e-14
		 -7.1054273576010019e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.020892901461180398 0.14462672919893782 -0.088676289376428963 0.98528326463620519 1
		 1 1 yes;
	setAttr ".xm[28]" -type "matrix" "xform" 1 1 1 0 0 0 0 7.5 -5.7999999999999998
		 16 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.46912892690605035 -0.50139121917991458 -0.53086007674465763 0.49670159471045872 1
		 1 1 yes;
	setAttr ".xm[29]" -type "matrix" "xform" 1 1 1 0 0 0 0 10.540398474441089 7.1054273576010019e-15
		 1.7763568394002505e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.00077449584940481844 -0.033626991351148584 0.043393945532247971 0.99849165800220552 1
		 1 1 yes;
	setAttr ".xm[30]" -type "matrix" "xform" 1 1 1 0 0 0 0 7.5 -5.7999999999999998
		 -16 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.50139121917991414 -0.46912892690605068 0.49670159471045838 0.53086007674465807 1
		 1 1 yes;
	setAttr ".xm[31]" -type "matrix" "xform" 1 1 1 0 0 0 0 -10.540398474441089 -1.4210854715202004e-14
		 -2.2204460492503131e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.00077449584947627691 -0.033626991351145587 0.043393945532250282 0.99849165800220541 1
		 1 1 yes;
	setAttr ".xm[32]" -type "matrix" "xform" 1 1 1 0 0 0 0 9.8025506884687879 -7.1054273576010019e-15
		 2.6645352591003757e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.00042305059045699695 0.025085007565926146 -0.0035400666623068241 0.99967896414381041 1
		 1 1 yes;
	setAttr ".xm[33]" -type "matrix" "xform" 1 1 1 0 0 0 0 7.5346388505817359 2.0998136563286565
		 5.5377924468302808e-13 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -6.3644292697138475e-05 0.0017349362928195812 -0.049968493303026981 0.99874928566807442 1
		 1 1 yes;
	setAttr ".xm[34]" -type "matrix" "xform" 1 1 1 0 0 0 0 -9.8025506884687879 7.1054273576010019e-15
		 -1.7763568394002505e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.00042305059078482737 0.025085007565924484 -0.0035400666623186094 0.99967896414381041 1
		 1 1 yes;
	setAttr ".xm[35]" -type "matrix" "xform" 1 1 1 0 0 0 0 -7.5346388505817359 -2.0998136563286565
		 1.1368683772161603e-12 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -6.3644295307225439e-05 0.0017349362929103191 -0.049968493303023831 0.99874928566807442 1
		 1 1 yes;
	setAttr ".xm[36]" -type "matrix" "xform" 1 1 1 0 0 0 0 2.3108440016582819 0
		 4.4408920985006262e-16 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0029304941790832323 -0.044325698295910945 0.020618355383041569 0.99880004410079248 1
		 1 1 yes;
	setAttr ".xm[37]" -type "matrix" "xform" 1 1 1 0 0 0 0 -2.3108440016582747 -2.1316282072803006e-14
		 5.4178883601707639e-14 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0029304941813225651 -0.044325698295956069 0.020618355382944566 0.99880004410078582 1
		 1 1 yes;
	setAttr ".xm[38]" -type "matrix" "xform" 1 1 1 0 0 0 0 7.4786885620873278 -2.0998136563286707
		 -5.6088467204062908e-13 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -6.3644292697138475e-05 0.0017349362928196 -0.049968493303027529 0.99874928566807442 1
		 1 1 yes;
	setAttr ".xm[39]" -type "matrix" "xform" 1 1 1 0 0 0 0 -7.4786885620873136 2.0998136563286707
		 -1.1306511282782594e-12 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -6.3644295742239958e-05 0.0017349362929321007 -0.049968493303023588 0.99874928566807419 1
		 1 1 yes;
	setAttr ".xm[40]" -type "matrix" "xform" 1 1 1 0 0 0 0 2.3108440016582676 7.1054273576010019e-15
		 8.8817841970012523e-16 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0013582112171582447 -0.069132741284131272 0.066802565401857958 0.99536738775216138 1
		 1 1 yes;
	setAttr ".xm[41]" -type "matrix" "xform" 1 1 1 0 0 0 0 -2.3108440016582676 -1.4210854715202004e-14
		 5.4178883601707639e-14 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.0013582112199117691 -0.069132741284300539 0.066802565401682737 0.9953673877521575 1
		 1 1 yes;
	setAttr ".xm[42]" -type "matrix" "xform" 1 1 1 0 0 0 0 2.0168746852511958 3.4734404254248261
		 0.77937674780391841 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.65350911613567575 0.23607997082320387 0.44968632009162701 0.56122570862900945 1
		 1 1 yes;
	setAttr ".xm[43]" -type "matrix" "xform" 1 1 1 0 0 0 0 -2.0168746852512029 -3.4734404254254656
		 -0.77937674780112065 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.65350911613545193 0.23607997082338317 0.44968632009153292 0.56122570862927013 1
		 1 1 yes;
	setAttr ".xm[44]" -type "matrix" "xform" 1 1 1 0 0 0 0 3.2939338184001343 0
		 -1.7763568394002505e-15 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.017435183181781895 0.075151639664309502 -0.080162911744626877 0.99379180567350556 1
		 1 1 yes;
	setAttr ".xm[45]" -type "matrix" "xform" 1 1 1 0 0 0 0 -3.2939338184001272 -7.3541173151170369e-13
		 -1.2789769243681803e-13 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.017435183181781888 0.075151639664309502 -0.080162911744626836 0.99379180567350556 1
		 1 1 yes;
	setAttr -s 46 ".m";
	setAttr -s 46 ".p";
	setAttr -s 46 ".g[0:45]" yes no no no no no no no no no no no no no 
		no no no no no no no no no no no no no no no no no no no no no no no no no no no 
		no no no no no;
	setAttr ".bp" yes;
createNode dagPose -n "bindPose2";
	rename -uid "89F6D9B9-41BE-E66C-D0FB-569F5F39A5E8";
	setAttr -s 5 ".wm";
	setAttr ".wm[0]" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 11 1 1;
	setAttr ".wm[1]" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 11 1 1;
	setAttr ".wm[2]" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 19 1 1;
	setAttr ".wm[3]" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 28 1 1;
	setAttr ".wm[4]" -type "matrix" 0.9659258262890682 0 -0.25881904510252074 0 0 1 0 0
		 0.25881904510252074 0 0.9659258262890682 0 7.6100000000000003 38.173999999999999 9.4616000000000007 1;
	setAttr -s 5 ".xm";
	setAttr ".xm[0]" -type "matrix" "xform" 1 1 1 0 0 0 0 0 11 1 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0.50000000000000011 0.50000000000000011 0.50000000000000011 0.50000000000000011 1
		 1 1 yes;
	setAttr ".xm[1]" -type "matrix" "xform" 1 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[2]" -type "matrix" "xform" 1 1 1 0 0 0 0 8 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[3]" -type "matrix" "xform" 1 1 1 0 0 0 0 9 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[4]" -type "matrix" "xform" 1 1 1 0 0 0 0 10.173999999999999 8.4616000000000007
		 7.6100000000000003 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 -0.43045933457687946 -0.43045933457687946 -0.56098552679693103 0.56098552679693103 1
		 1 1 yes;
	setAttr -s 5 ".m";
	setAttr -s 5 ".p";
	setAttr -s 5 ".g[0:4]" yes yes yes yes no;
	setAttr ".bp" yes;
createNode dagPose -n "bindPose3";
	rename -uid "EBDF8B0B-4547-2893-77ED-3A841BF407D6";
	setAttr -s 5 ".wm";
	setAttr ".wm[0]" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 11 1 1;
	setAttr ".wm[1]" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 11 1 1;
	setAttr ".wm[2]" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 19 1 1;
	setAttr ".wm[3]" -type "matrix" 0 1 0 0 0 0 1 0 1 0 0 0 0 28 1 1;
	setAttr ".wm[4]" -type "matrix" 0.96592582628906842 -2.2204460492503131e-16 0.25881904510252079 0
		 1.1102230246251565e-16 1.0000000000000002 -2.2204460492503131e-16 0 -0.25881904510252096 1.1102230246251565e-16 0.96592582628906842 0
		 -7.6100000000000003 38.173999999999999 9.4616000000000007 1;
	setAttr -s 5 ".xm";
	setAttr ".xm[0]" -type "matrix" "xform" 1 1 1 0 0 0 0 0 11 1 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0.50000000000000011 0.50000000000000011 0.50000000000000011 0.50000000000000011 1
		 1 1 yes;
	setAttr ".xm[1]" -type "matrix" "xform" 1 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[2]" -type "matrix" "xform" 1 1 1 0 0 0 0 8 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[3]" -type "matrix" "xform" 1 1 1 0 0 0 0 9 0 0 0 0 0 0 0 0 0 0
		 0 0 0 0 0 0 0 0 0 0 1 0 0 0 1 1 1 1 yes;
	setAttr ".xm[4]" -type "matrix" "xform" 1 1 1 0 0 0 0 10.173999999999999 8.4616000000000007
		 -7.6100000000000003 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0.56098552679693103 0.56098552679693103 0.43045933457687952 -0.4304593345768794 1
		 1 1 yes;
	setAttr -s 5 ".m";
	setAttr -s 5 ".p";
	setAttr -s 5 ".g[0:4]" yes yes yes yes no;
	setAttr ".bp" yes;
createNode groupParts -n "buddy_body_skinGroupParts";
	rename -uid "738250F0-45E0-F1B7-4908-05B4D79E1D48";
	setAttr ".ihi" 0;
	setAttr ".ic" -type "componentList" 2 "vtx[1280]" "vtx[1291]";
createNode materialInfo -n "materialInfo2";
	rename -uid "C051C05A-48F1-B083-5578-34A29C1A8989";
createNode shadingEngine -n "lambert3SG";
	rename -uid "CBDE962E-4770-B543-5256-FDBBAB5CF736";
	setAttr ".ihi" 0;
	setAttr ".ro" yes;
createNode lambert -n "lambert3";
	rename -uid "72605B33-4288-989E-E4A5-A8AAA9B01BBA";
	setAttr ".c" -type "float3" 0.20702501 0.45871794 0.63700002 ;
	setAttr ".ambc" -type "float3" 0 0.13293593 0.32061067 ;
createNode skinCluster -n "buddy_teeth_skin";
	rename -uid "6BBB6292-4F36-8080-3AF7-F5809ECB5700";
	setAttr -s 132 ".wl";
	setAttr ".wl[0:131].w"
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 0.99999999999999989
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 0.99999999999999989
		1 2 1
		1 2 1
		1 2 1
		1 2 1.0000000000000002
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 0.99999999999999989
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 0.99999999999999989
		1 2 0.99999999999999989
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 0.99999999999999989
		1 2 1
		1 2 0.99999999999999989
		1 2 1
		1 2 0.99999999999999989
		1 2 1
		1 2 1
		1 2 1
		1 2 1;
	setAttr -s 45 ".pm";
	setAttr ".pm[0]" -type "matrix" -4.9445965714596328e-16 -2.3592239273284553e-15 -1 0
		 3.655141400502731e-15 0.99999999999999978 -2.3592239273284592e-15 0 1 -3.5982221934442676e-15 -5.021409014621135e-16 0
		 -5.7322045408835608e-15 -1.2999999999999792 8.5000000000000053 1;
	setAttr ".pm[1]" -type "matrix" -9.1983392873923308e-15 -9.1732177409653244e-15 -0.99999999999999978 0
		 -2.9400723240728159e-15 -0.99999999999999978 9.1593399531575635e-15 0 -0.99999999999999978 2.9456351175170989e-15 9.1874070015623072e-15 0
		 -6.9077569921540175e-14 1.299999999999921 -8.5000000000000107 1;
	setAttr ".pm[2]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -28 -1 0 1;
	setAttr ".pm[3]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -11 -1 0 1;
	setAttr ".pm[4]" -type "matrix" 0.080815868766054871 -2.6547615831701794e-16 -0.99672904811467566 0
		 -0.99672904811467555 7.6856153647732499e-16 -0.080815868766054871 0 7.4045367094602878e-16 1 -2.0276607625338074e-16 0
		 4.3209551166917279 -1.6659031841870784e-15 8.5772575383706133 1;
	setAttr ".pm[5]" -type "matrix" 0.080815868766054788 -8.9277224251399205e-15 -0.99672904811467566 0
		 0.99672904811467589 3.9059674980265064e-16 0.080815868766054774 0 -3.4781929143902126e-16 -0.99999999999999978 8.9262402681233043e-15 0
		 -4.3209551166917297 -7.5049285332698075e-14 -8.5772575383706133 1;
	setAttr ".pm[6]" -type "matrix" 0.098742859722262016 -0.012281602211417567 -0.99503719020998915 0
		 -0.98742859722262477 0.12281602211417433 -0.099503719020998471 0 0.12342857465282848 0.99235345868252578 -1.8908485888147192e-16 0
		 8.1783773564963926 -0.51337097243724905 8.6568235548269019 1;
	setAttr ".pm[7]" -type "matrix" 0.098742859722262349 -0.012281602211426407 -0.99503719020998882 0
		 0.98742859722262477 -0.12281602211417386 0.099503719020999887 0 -0.1234285746528289 -0.99235345868252545 8.9164786665207837e-15 0
		 -8.1783773564963926 0.51337097243717611 -8.6568235548269072 1;
	setAttr ".pm[8]" -type "matrix" 0.99973690272738092 0.022934649941631899 -0.00035659096746199154 0
		 -0.022934651399784185 0.99973696628921893 2.0736088220856333e-14 0 0.00035649717201706059 8.1782895103967676e-06 0.99999993642143903 0
		 -25.148314410141758 -19.302845123270217 1.6771232738530868 1;
	setAttr ".pm[9]" -type "matrix" 0.99973690272738114 0.022934649941631972 -0.00035659096746026039 0
		 0.022934651399784057 -0.99973696628921904 -1.0346657883762716e-13 0 -0.00035649717201719579 -8.178289427662096e-06 -0.99999993642143936 0
		 25.148314410141765 19.302845123270359 -1.6771232738514947 1;
	setAttr ".pm[10]" -type "matrix" 0.96687323370463074 -0.0064362448361374698 0.25517587013265941 0
		 0.00665661434564569 0.99997784449729377 -6.6394372638667336e-15 0 -0.25517021658297795 0.0016986073577941775 0.9668946557417929 0
		 -39.010218020314554 -17.973722239951002 -8.5538951272865571 1;
	setAttr ".pm[11]" -type "matrix" 0.96687323370463096 -0.006436244836136037 0.25517587013265902 0
		 -0.0066566143456458903 -0.99997784449729354 1.1921670248216114e-14 0 0.25517021658297773 -0.0016986073577993416 -0.96689465574179312 0
		 39.010218020314561 17.973722239951041 8.5538951272864363 1;
	setAttr ".pm[12]" -type "matrix" 0.92663734741845671 -0.069700799886994264 0.36943879718351202 0
		 0.075007177835449343 0.99718299387482645 -6.3213323464594882e-15 0 -0.36839808582896871 0.027710561559664108 0.92925506462736074 0
		 -40.628326878558063 -15.240516488963546 -13.378538417879168 1;
	setAttr ".pm[13]" -type "matrix" 0.92663734741845682 -0.069700799887000564 0.36943879718351041 0
		 -0.075007177835449523 -0.99718299387482601 -1.0907941216942168e-14 0 0.36839808582896849 -0.027710561559648118 -0.92925506462736129 0
		 40.628326878558077 15.240516488963305 13.37853841787941 1;
	setAttr ".pm[14]" -type "matrix" 0.95236204559806781 0.17211128701015596 0.25176226680734287 0
		 -0.17783966080066863 0.98405947739265409 -2.081668171172169e-16 0 -0.24774904470162371 -0.044773316131425024 0.96778910978168609 0
		 -35.554435969599758 -21.188748558206171 -8.5496602517730622 1;
	setAttr ".pm[15]" -type "matrix" 0.95236204559806792 0.17211128701015616 0.25176226680734265 0
		 0.17783966080066815 -0.98405947739265431 1.8457457784393235e-15 0 0.24774904470162393 0.044773316131423255 -0.96778910978168631 0
		 35.554435969599758 21.188748558206171 8.5496602517729379 1;
	setAttr ".pm[16]" -type "matrix" 0.96063681300616799 0.23201230989430555 0.15279791083277747 0
		 -0.23476909644539859 0.97205116704534211 -2.0816681711721691e-17 0 -0.14852738754709141 -0.035872227464955678 0.98825745554745936 0
		 -36.528353759534973 -23.469678737291968 -4.4324184805332845 1;
	setAttr ".pm[17]" -type "matrix" 0.9606368130061681 0.23201230989430446 0.15279791083277891 0
		 0.23476909644539867 -0.97205116704534189 -8.8193341518660888e-15 0 0.14852738754709088 0.035872227464964469 -0.98825745554745925 0
		 36.52835375953498 23.469678737291936 4.4324184805333875 1;
	setAttr ".pm[18]" -type "matrix" 0.99368106829059399 0.084974522483502749 0.073329837375820633 0
		 -0.085203913285456678 0.99636353464026595 1.7347234759768066e-16 0 -0.073063175962368526 -0.0062479891050061598 0.99730774335228911 0
		 -14.807324493690764 -20.837416535567982 -0.21263457334875474 1;
	setAttr ".pm[19]" -type "matrix" 0.99368106829059433 0.08497452248350261 0.073329837375820647 0
		 0.085203913285456304 -0.99636353464026617 4.657732532997728e-16 0 0.073063175962368609 0.006247989105005485 -0.99730774335228944 0
		 14.807324493690775 20.837416535567989 0.2126345733487422 1;
	setAttr ".pm[20]" -type "matrix" 0.47542194344605976 -0.11293657638623206 0.87247882805494381 0
		 0.84885696101420938 -0.2016461384253353 -0.48865191557577264 0 0.23111866092484354 0.97292556990363188 -1.1102230246251565e-16 0
		 -35.314075940043388 9.0682599121482781 -20.458691470047007 1;
	setAttr ".pm[21]" -type "matrix" 0.47542194344605937 -0.1129365763862317 0.87247882805494448 0
		 -0.84885696101420927 0.20164613842533655 0.48865191557577164 0 -0.23111866092484443 -0.97292556990363188 8.1878948066105295e-16 0
		 35.314075940043374 -9.0682599121482212 20.45869147004705 1;
	setAttr ".pm[22]" -type "matrix" 0.21522222383529763 -0.011570577349480157 0.97649655202012264 0
		 0.97508844268237926 -0.052421799420097268 -0.2155330227431782 0 0.053683547895432872 0.99855799865874495 -6.5052130349130236e-16 0
		 -31.335040543496159 1.7496992184419178 -30.849835360702269 1;
	setAttr ".pm[23]" -type "matrix" 0.21522222383529768 -0.011570577349479749 0.97649655202012309 0
		 -0.97508844268237926 0.052421799420098239 0.2155330227431779 0 -0.053683547895433649 -0.99855799865874528 1.1726730697603216e-15 0
		 31.335040543496135 -1.7496992184418749 30.849835360702286 1;
	setAttr ".pm[24]" -type "matrix" 0.93616767052418659 0.35114534289864113 -0.01694227924199633 0
		 -0.35119575027525313 0.93630205862670335 -7.6527326142716845e-15 0 0.015863090932106909 0.0059500564697729796 0.99985646928651029 0
		 -25.294461554979442 -29.276137066081382 2.2407646310315723 1;
	setAttr ".pm[25]" -type "matrix" 0.93616767052418692 0.35114534289864363 -0.016942279241942765 0
		 0.35119575027525263 -0.93630205862670346 -1.349051079180264e-13 0 -0.015863090932106784 -0.0059500564696207073 -0.9998564692865114 0
		 25.294461554979474 29.276137066081731 -2.2407646310271123 1;
	setAttr ".pm[26]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -19 -1 0 1;
	setAttr ".pm[27]" -type "matrix" 0.99973979339155139 -0.020401790803298011 -0.010203550432900537 0
		 0.020402852926359051 0.99979184013096711 -7.6992719925256248e-15 0 0.010201426463179043 -0.00020818153880135116 0.9999479424242862 0
		 -27.148036103812238 -34.253232760579778 5.6701129755632778 1;
	setAttr ".pm[28]" -type "matrix" 0.99973979339155117 -0.020401790803299479 -0.010203550432897578 0
		 -0.020402852926358871 -0.99979184013096689 1.4947258383281307e-13 0 -0.010201426463179014 0.0002081815386597814 -0.99994794242428597 0
		 27.148036103812224 34.253232760578982 -5.6701129755681317 1;
	setAttr ".pm[29]" -type "matrix" 0.99530734153820533 0.08646677643655834 0.043437224276190865 0
		 -0.086548464481594617 0.99624764155097389 -1.075397149802315e-12 0 -0.043274232240768741 -0.0037594250613764936 0.99905615835506445 0
		 -40.617194381206382 -40.868709853961192 3.6921640635311892 1;
	setAttr ".pm[30]" -type "matrix" 0.99530734153820521 0.086466776436365869 0.043437224276573892 0
		 0.086548464481594742 -0.99624764155097401 -3.3370112229036392e-12 0 0.043274232240768755 0.0037594250658015767 -0.99905615835504769 0
		 40.61719438120636 40.868709853977556 -3.6921640633501926 1;
	setAttr ".pm[31]" -type "matrix" 0.9979402655783316 0.045314133196497637 -0.045407660918601073 0
		 -0.045360921162700261 0.9989706636489738 5.1230721054285048e-15 0 0.045360921162602853 0.0020597333271062924 0.99896854021030179 0
		 -44.068134909514349 -39.039155615406038 7.7874138475459711 1;
	setAttr ".pm[32]" -type "matrix" 0.9979402655783316 0.045314133196495188 -0.045407660918603564 0
		 0.045360921162700518 -0.99897066364897391 4.9820390868315899e-14 0 -0.045360921162602874 -0.0020597333271610203 -0.99896854021030179 0
		 44.068134909514342 39.039155615405647 -7.7874138475481889 1;
	setAttr ".pm[33]" -type "matrix" 0.99530734153820521 0.086466776436559437 0.043437224276190907 0
		 -0.086548464481595699 0.99624764155097389 -1.0753993182066599e-12 0 -0.043274232240768769 -0.0037594250613765413 0.99905615835506456 0
		 -40.980697932029045 -36.684469759447154 3.6921640635267039 1;
	setAttr ".pm[34]" -type "matrix" 0.9953073415382051 0.086466776436329065 0.043437224276649249 0
		 0.086548464481595797 -0.99624764155097378 -4.2048569487418403e-12 0 0.04327423224076879 0.0037594250666719178 -0.99905615835504447 0
		 40.980697932029031 36.684469759466722 -3.6921640633322506 1;
	setAttr ".pm[35]" -type "matrix" 0.99437882000179478 -0.047138076826326376 -0.094809092628046168 0
		 0.04735137238093217 0.99887829465538125 1.6278645098566358e-14 0 0.094702744762126229 -0.0044893406501449974 0.99549547259394744 0
		 -46.854182971033204 -30.615731964708665 10.045023363937858 1;
	setAttr ".pm[36]" -type "matrix" 0.99437882000179478 -0.047138076826348421 -0.094809092628035191 0
		 -0.047351372380931955 -0.99887829465538114 2.1597133803563651e-13 0 -0.094702744762126173 0.0044893406499137293 -0.99549547259394855 0
		 46.85418297103319 30.615731964706342 -10.045023363945013 1;
	setAttr ".pm[37]" -type "matrix" 0.99616727256193882 0.066302989947811045 0.057049790538513126 0
		 -0.066411151504129767 0.99779234260235494 2.2941717969793269e-16 0 -0.056923844146396542 -0.0037887422827321735 0.99837133442397663 0
		 -13.854314934497115 -36.500661964505689 3.8793857566188681 1;
	setAttr ".pm[38]" -type "matrix" 0.99616727256193882 0.066302989947810836 0.057049790538513376 0
		 0.066411151504129851 -0.99779234260235494 -1.5452049362263405e-15 0 0.05692384414639666 0.00378874228273373 -0.99837133442397663 0
		 13.854314934497113 36.500661964505696 -3.8793857566188183 1;
	setAttr ".pm[39]" -type "matrix" 0.48574139257513332 -0.15477007687694017 0.86029153363419564 0
		 0.81968859997061561 -0.26117450472986165 -0.50980239030169672 0 0.30358837035924591 0.95280328577446549 -1.1102230246251568e-16 0
		 -48.76539993084377 20.365807491048994 -13.321773711574373 1;
	setAttr ".pm[40]" -type "matrix" 0.4857413925751331 -0.15477007687694019 0.86029153363419564 0
		 -0.81968859997061538 0.2611745047298617 0.50980239030169694 0 -0.30358837035924585 -0.95280328577446527 -8.3266726846886704e-17 0
		 48.765399930843763 -20.365807491049026 13.321773711574361 1;
	setAttr ".pm[41]" -type "matrix" 0.36735917918532551 -0.054573509608920705 0.9284766908852633 0
		 0.91839794796334273 -0.13643377402230694 -0.37139067635409329 0 0.14694367167389333 0.9891448616633437 -3.0531133177191805e-16 0
		 -51.966629487558329 11.359476025103717 -21.744924100533268 1;
	setAttr ".pm[42]" -type "matrix" 0.36735917918532535 -0.054573509608920788 0.92847669088526319 0
		 -0.91839794796334262 0.13643377402230716 0.37139067635409351 0 -0.14694367167389336 -0.98914486166334337 6.2450045135165031e-17 0
		 51.966629487558194 -11.35947602510303 21.744924100533339 1;
	setAttr ".pm[43]" -type "matrix" 0.99911229454319339 -0.013310852834388294 0.03996803834886755 0
		 0.013321497260576886 0.99991126491841087 2.5778728110492644e-13 0 -0.039964491781727007 0.00053243411311762186 0.99920095872178971 0
		 -36.945840502481261 -34.51088841345657 3.8449252891525707 1;
	setAttr ".pm[44]" -type "matrix" 0.99911229454319328 -0.01331085283435644 0.039968038348878215 0
		 -0.013321497260576709 -0.99991126491841076 5.398250206220533e-13 0 0.039964491781727035 -0.00053243411391443315 -0.99920095872178927 0
		 36.945840502481246 34.510888413453493 -3.8449252891800985 1;
	setAttr ".gm" -type "matrix" 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1;
	setAttr -s 45 ".ma";
	setAttr -s 45 ".dpf[0:44]"  4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 
		4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4;
	setAttr -s 45 ".lw";
	setAttr -s 45 ".lw";
	setAttr ".mmi" yes;
	setAttr ".mi" 4;
	setAttr ".bm" 1;
	setAttr ".ucm" yes;
	setAttr -s 45 ".ifcl";
	setAttr -s 45 ".ifcl";
createNode materialInfo -n "materialInfo4";
	rename -uid "8132CD15-4121-B606-6154-5CB355C92373";
createNode shadingEngine -n "lambert5SG";
	rename -uid "A2BF2C51-4989-DA4D-F7F9-3CB9AF2C1AB1";
	setAttr ".ihi" 0;
	setAttr -s 3 ".dsm";
	setAttr ".ro" yes;
	setAttr -s 2 ".gn";
createNode lambert -n "lambert5";
	rename -uid "7490AC85-4F91-C65D-DC6F-688331598E2E";
	setAttr ".c" -type "float3" 0.8206107 0.8206107 0.8206107 ;
	setAttr ".ambc" -type "float3" 0.22900763 0.22900763 0.22900763 ;
createNode skinCluster -n "buddy_tongue_skin";
	rename -uid "74124C41-4335-BAA5-560B-7E90B08C7206";
	setAttr -s 26 ".wl";
	setAttr ".wl[0:25].w"
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.99763192586173188 26 0.0023680741382681456
		1 2 0.99999999999999989
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		1 2 1
		2 2 0.99108295709158034 26 0.0089170429084196035
		1 2 1
		1 2 1;
	setAttr -s 45 ".pm";
	setAttr ".pm[0]" -type "matrix" -4.9445965714596328e-16 -2.3592239273284553e-15 -1 0
		 3.655141400502731e-15 0.99999999999999978 -2.3592239273284592e-15 0 1 -3.5982221934442676e-15 -5.021409014621135e-16 0
		 -5.7322045408835608e-15 -1.2999999999999792 8.5000000000000053 1;
	setAttr ".pm[1]" -type "matrix" -9.1983392873923308e-15 -9.1732177409653244e-15 -0.99999999999999978 0
		 -2.9400723240728159e-15 -0.99999999999999978 9.1593399531575635e-15 0 -0.99999999999999978 2.9456351175170989e-15 9.1874070015623072e-15 0
		 -6.9077569921540175e-14 1.299999999999921 -8.5000000000000107 1;
	setAttr ".pm[2]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -28 -1 0 1;
	setAttr ".pm[3]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -11 -1 0 1;
	setAttr ".pm[4]" -type "matrix" 0.080815868766054871 -2.6547615831701794e-16 -0.99672904811467566 0
		 -0.99672904811467555 7.6856153647732499e-16 -0.080815868766054871 0 7.4045367094602878e-16 1 -2.0276607625338074e-16 0
		 4.3209551166917279 -1.6659031841870784e-15 8.5772575383706133 1;
	setAttr ".pm[5]" -type "matrix" 0.080815868766054788 -8.9277224251399205e-15 -0.99672904811467566 0
		 0.99672904811467589 3.9059674980265064e-16 0.080815868766054774 0 -3.4781929143902126e-16 -0.99999999999999978 8.9262402681233043e-15 0
		 -4.3209551166917297 -7.5049285332698075e-14 -8.5772575383706133 1;
	setAttr ".pm[6]" -type "matrix" 0.098742859722262016 -0.012281602211417567 -0.99503719020998915 0
		 -0.98742859722262477 0.12281602211417433 -0.099503719020998471 0 0.12342857465282848 0.99235345868252578 -1.8908485888147192e-16 0
		 8.1783773564963926 -0.51337097243724905 8.6568235548269019 1;
	setAttr ".pm[7]" -type "matrix" 0.098742859722262349 -0.012281602211426407 -0.99503719020998882 0
		 0.98742859722262477 -0.12281602211417386 0.099503719020999887 0 -0.1234285746528289 -0.99235345868252545 8.9164786665207837e-15 0
		 -8.1783773564963926 0.51337097243717611 -8.6568235548269072 1;
	setAttr ".pm[8]" -type "matrix" 0.99973690272738092 0.022934649941631899 -0.00035659096746199154 0
		 -0.022934651399784185 0.99973696628921893 2.0736088220856333e-14 0 0.00035649717201706059 8.1782895103967676e-06 0.99999993642143903 0
		 -25.148314410141758 -19.302845123270217 1.6771232738530868 1;
	setAttr ".pm[9]" -type "matrix" 0.99973690272738114 0.022934649941631972 -0.00035659096746026039 0
		 0.022934651399784057 -0.99973696628921904 -1.0346657883762716e-13 0 -0.00035649717201719579 -8.178289427662096e-06 -0.99999993642143936 0
		 25.148314410141765 19.302845123270359 -1.6771232738514947 1;
	setAttr ".pm[10]" -type "matrix" 0.96687323370463074 -0.0064362448361374698 0.25517587013265941 0
		 0.00665661434564569 0.99997784449729377 -6.6394372638667336e-15 0 -0.25517021658297795 0.0016986073577941775 0.9668946557417929 0
		 -39.010218020314554 -17.973722239951002 -8.5538951272865571 1;
	setAttr ".pm[11]" -type "matrix" 0.96687323370463096 -0.006436244836136037 0.25517587013265902 0
		 -0.0066566143456458903 -0.99997784449729354 1.1921670248216114e-14 0 0.25517021658297773 -0.0016986073577993416 -0.96689465574179312 0
		 39.010218020314561 17.973722239951041 8.5538951272864363 1;
	setAttr ".pm[12]" -type "matrix" 0.92663734741845671 -0.069700799886994264 0.36943879718351202 0
		 0.075007177835449343 0.99718299387482645 -6.3213323464594882e-15 0 -0.36839808582896871 0.027710561559664108 0.92925506462736074 0
		 -40.628326878558063 -15.240516488963546 -13.378538417879168 1;
	setAttr ".pm[13]" -type "matrix" 0.92663734741845682 -0.069700799887000564 0.36943879718351041 0
		 -0.075007177835449523 -0.99718299387482601 -1.0907941216942168e-14 0 0.36839808582896849 -0.027710561559648118 -0.92925506462736129 0
		 40.628326878558077 15.240516488963305 13.37853841787941 1;
	setAttr ".pm[14]" -type "matrix" 0.95236204559806781 0.17211128701015596 0.25176226680734287 0
		 -0.17783966080066863 0.98405947739265409 -2.081668171172169e-16 0 -0.24774904470162371 -0.044773316131425024 0.96778910978168609 0
		 -35.554435969599758 -21.188748558206171 -8.5496602517730622 1;
	setAttr ".pm[15]" -type "matrix" 0.95236204559806792 0.17211128701015616 0.25176226680734265 0
		 0.17783966080066815 -0.98405947739265431 1.8457457784393235e-15 0 0.24774904470162393 0.044773316131423255 -0.96778910978168631 0
		 35.554435969599758 21.188748558206171 8.5496602517729379 1;
	setAttr ".pm[16]" -type "matrix" 0.96063681300616799 0.23201230989430555 0.15279791083277747 0
		 -0.23476909644539859 0.97205116704534211 -2.0816681711721691e-17 0 -0.14852738754709141 -0.035872227464955678 0.98825745554745936 0
		 -36.528353759534973 -23.469678737291968 -4.4324184805332845 1;
	setAttr ".pm[17]" -type "matrix" 0.9606368130061681 0.23201230989430446 0.15279791083277891 0
		 0.23476909644539867 -0.97205116704534189 -8.8193341518660888e-15 0 0.14852738754709088 0.035872227464964469 -0.98825745554745925 0
		 36.52835375953498 23.469678737291936 4.4324184805333875 1;
	setAttr ".pm[18]" -type "matrix" 0.99368106829059399 0.084974522483502749 0.073329837375820633 0
		 -0.085203913285456678 0.99636353464026595 1.7347234759768066e-16 0 -0.073063175962368526 -0.0062479891050061598 0.99730774335228911 0
		 -14.807324493690764 -20.837416535567982 -0.21263457334875474 1;
	setAttr ".pm[19]" -type "matrix" 0.99368106829059433 0.08497452248350261 0.073329837375820647 0
		 0.085203913285456304 -0.99636353464026617 4.657732532997728e-16 0 0.073063175962368609 0.006247989105005485 -0.99730774335228944 0
		 14.807324493690775 20.837416535567989 0.2126345733487422 1;
	setAttr ".pm[20]" -type "matrix" 0.47542194344605976 -0.11293657638623206 0.87247882805494381 0
		 0.84885696101420938 -0.2016461384253353 -0.48865191557577264 0 0.23111866092484354 0.97292556990363188 -1.1102230246251565e-16 0
		 -35.314075940043388 9.0682599121482781 -20.458691470047007 1;
	setAttr ".pm[21]" -type "matrix" 0.47542194344605937 -0.1129365763862317 0.87247882805494448 0
		 -0.84885696101420927 0.20164613842533655 0.48865191557577164 0 -0.23111866092484443 -0.97292556990363188 8.1878948066105295e-16 0
		 35.314075940043374 -9.0682599121482212 20.45869147004705 1;
	setAttr ".pm[22]" -type "matrix" 0.21522222383529763 -0.011570577349480157 0.97649655202012264 0
		 0.97508844268237926 -0.052421799420097268 -0.2155330227431782 0 0.053683547895432872 0.99855799865874495 -6.5052130349130236e-16 0
		 -31.335040543496159 1.7496992184419178 -30.849835360702269 1;
	setAttr ".pm[23]" -type "matrix" 0.21522222383529768 -0.011570577349479749 0.97649655202012309 0
		 -0.97508844268237926 0.052421799420098239 0.2155330227431779 0 -0.053683547895433649 -0.99855799865874528 1.1726730697603216e-15 0
		 31.335040543496135 -1.7496992184418749 30.849835360702286 1;
	setAttr ".pm[24]" -type "matrix" 0.93616767052418659 0.35114534289864113 -0.01694227924199633 0
		 -0.35119575027525313 0.93630205862670335 -7.6527326142716845e-15 0 0.015863090932106909 0.0059500564697729796 0.99985646928651029 0
		 -25.294461554979442 -29.276137066081382 2.2407646310315723 1;
	setAttr ".pm[25]" -type "matrix" 0.93616767052418692 0.35114534289864363 -0.016942279241942765 0
		 0.35119575027525263 -0.93630205862670346 -1.349051079180264e-13 0 -0.015863090932106784 -0.0059500564696207073 -0.9998564692865114 0
		 25.294461554979474 29.276137066081731 -2.2407646310271123 1;
	setAttr ".pm[26]" -type "matrix" 0 0 1 0 1 0 0 0 0 1 0 0 -19 -1 0 1;
	setAttr ".pm[27]" -type "matrix" 0.99973979339155139 -0.020401790803298011 -0.010203550432900537 0
		 0.020402852926359051 0.99979184013096711 -7.6992719925256248e-15 0 0.010201426463179043 -0.00020818153880135116 0.9999479424242862 0
		 -27.148036103812238 -34.253232760579778 5.6701129755632778 1;
	setAttr ".pm[28]" -type "matrix" 0.99973979339155117 -0.020401790803299479 -0.010203550432897578 0
		 -0.020402852926358871 -0.99979184013096689 1.4947258383281307e-13 0 -0.010201426463179014 0.0002081815386597814 -0.99994794242428597 0
		 27.148036103812224 34.253232760578982 -5.6701129755681317 1;
	setAttr ".pm[29]" -type "matrix" 0.99530734153820533 0.08646677643655834 0.043437224276190865 0
		 -0.086548464481594617 0.99624764155097389 -1.075397149802315e-12 0 -0.043274232240768741 -0.0037594250613764936 0.99905615835506445 0
		 -40.617194381206382 -40.868709853961192 3.6921640635311892 1;
	setAttr ".pm[30]" -type "matrix" 0.99530734153820521 0.086466776436365869 0.043437224276573892 0
		 0.086548464481594742 -0.99624764155097401 -3.3370112229036392e-12 0 0.043274232240768755 0.0037594250658015767 -0.99905615835504769 0
		 40.61719438120636 40.868709853977556 -3.6921640633501926 1;
	setAttr ".pm[31]" -type "matrix" 0.9979402655783316 0.045314133196497637 -0.045407660918601073 0
		 -0.045360921162700261 0.9989706636489738 5.1230721054285048e-15 0 0.045360921162602853 0.0020597333271062924 0.99896854021030179 0
		 -44.068134909514349 -39.039155615406038 7.7874138475459711 1;
	setAttr ".pm[32]" -type "matrix" 0.9979402655783316 0.045314133196495188 -0.045407660918603564 0
		 0.045360921162700518 -0.99897066364897391 4.9820390868315899e-14 0 -0.045360921162602874 -0.0020597333271610203 -0.99896854021030179 0
		 44.068134909514342 39.039155615405647 -7.7874138475481889 1;
	setAttr ".pm[33]" -type "matrix" 0.99530734153820521 0.086466776436559437 0.043437224276190907 0
		 -0.086548464481595699 0.99624764155097389 -1.0753993182066599e-12 0 -0.043274232240768769 -0.0037594250613765413 0.99905615835506456 0
		 -40.980697932029045 -36.684469759447154 3.6921640635267039 1;
	setAttr ".pm[34]" -type "matrix" 0.9953073415382051 0.086466776436329065 0.043437224276649249 0
		 0.086548464481595797 -0.99624764155097378 -4.2048569487418403e-12 0 0.04327423224076879 0.0037594250666719178 -0.99905615835504447 0
		 40.980697932029031 36.684469759466722 -3.6921640633322506 1;
	setAttr ".pm[35]" -type "matrix" 0.99437882000179478 -0.047138076826326376 -0.094809092628046168 0
		 0.04735137238093217 0.99887829465538125 1.6278645098566358e-14 0 0.094702744762126229 -0.0044893406501449974 0.99549547259394744 0
		 -46.854182971033204 -30.615731964708665 10.045023363937858 1;
	setAttr ".pm[36]" -type "matrix" 0.99437882000179478 -0.047138076826348421 -0.094809092628035191 0
		 -0.047351372380931955 -0.99887829465538114 2.1597133803563651e-13 0 -0.094702744762126173 0.0044893406499137293 -0.99549547259394855 0
		 46.85418297103319 30.615731964706342 -10.045023363945013 1;
	setAttr ".pm[37]" -type "matrix" 0.99616727256193882 0.066302989947811045 0.057049790538513126 0
		 -0.066411151504129767 0.99779234260235494 2.2941717969793269e-16 0 -0.056923844146396542 -0.0037887422827321735 0.99837133442397663 0
		 -13.854314934497115 -36.500661964505689 3.8793857566188681 1;
	setAttr ".pm[38]" -type "matrix" 0.99616727256193882 0.066302989947810836 0.057049790538513376 0
		 0.066411151504129851 -0.99779234260235494 -1.5452049362263405e-15 0 0.05692384414639666 0.00378874228273373 -0.99837133442397663 0
		 13.854314934497113 36.500661964505696 -3.8793857566188183 1;
	setAttr ".pm[39]" -type "matrix" 0.48574139257513332 -0.15477007687694017 0.86029153363419564 0
		 0.81968859997061561 -0.26117450472986165 -0.50980239030169672 0 0.30358837035924591 0.95280328577446549 -1.1102230246251568e-16 0
		 -48.76539993084377 20.365807491048994 -13.321773711574373 1;
	setAttr ".pm[40]" -type "matrix" 0.4857413925751331 -0.15477007687694019 0.86029153363419564 0
		 -0.81968859997061538 0.2611745047298617 0.50980239030169694 0 -0.30358837035924585 -0.95280328577446527 -8.3266726846886704e-17 0
		 48.765399930843763 -20.365807491049026 13.321773711574361 1;
	setAttr ".pm[41]" -type "matrix" 0.36735917918532551 -0.054573509608920705 0.9284766908852633 0
		 0.91839794796334273 -0.13643377402230694 -0.37139067635409329 0 0.14694367167389333 0.9891448616633437 -3.0531133177191805e-16 0
		 -51.966629487558329 11.359476025103717 -21.744924100533268 1;
	setAttr ".pm[42]" -type "matrix" 0.36735917918532535 -0.054573509608920788 0.92847669088526319 0
		 -0.91839794796334262 0.13643377402230716 0.37139067635409351 0 -0.14694367167389336 -0.98914486166334337 6.2450045135165031e-17 0
		 51.966629487558194 -11.35947602510303 21.744924100533339 1;
	setAttr ".pm[43]" -type "matrix" 0.99911229454319339 -0.013310852834388294 0.03996803834886755 0
		 0.013321497260576886 0.99991126491841087 2.5778728110492644e-13 0 -0.039964491781727007 0.00053243411311762186 0.99920095872178971 0
		 -36.945840502481261 -34.51088841345657 3.8449252891525707 1;
	setAttr ".pm[44]" -type "matrix" 0.99911229454319328 -0.01331085283435644 0.039968038348878215 0
		 -0.013321497260576709 -0.99991126491841076 5.398250206220533e-13 0 0.039964491781727035 -0.00053243411391443315 -0.99920095872178927 0
		 36.945840502481246 34.510888413453493 -3.8449252891800985 1;
	setAttr ".gm" -type "matrix" 5.994676720366078 0 0 0 0 2.4938140804848508 0 0 0 0 7.003606517275113 0
		 0 27.697321864198447 15.397355930458216 1;
	setAttr -s 45 ".ma";
	setAttr -s 45 ".dpf[0:44]"  4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 
		4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4;
	setAttr -s 45 ".lw";
	setAttr -s 45 ".lw";
	setAttr ".mmi" yes;
	setAttr ".mi" 4;
	setAttr ".bm" 1;
	setAttr ".ucm" yes;
	setAttr -s 45 ".ifcl";
	setAttr -s 45 ".ifcl";
createNode materialInfo -n "materialInfo3";
	rename -uid "94CC7006-40F7-115C-51AC-17AAFAF14AF0";
createNode shadingEngine -n "lambert4SG";
	rename -uid "832F5A65-44F3-8833-1806-B7B63CA86258";
	setAttr ".ihi" 0;
	setAttr ".ro" yes;
createNode lambert -n "lambert4";
	rename -uid "CA261442-4A5D-95B2-85B5-D1A46078EFA6";
	setAttr ".c" -type "float3" 0.86900002 0.30762598 0.60885 ;
	setAttr ".ambc" -type "float3" 0.076335877 0 0.033514008 ;
createNode groupId -n "groupId18";
	rename -uid "DBB75E2C-4DA8-7029-255A-ABB0017900A5";
	setAttr ".ihi" 0;
createNode groupId -n "groupId19";
	rename -uid "CC018DBC-4B26-BD93-D00A-4B80C1F1E5D6";
	setAttr ".ihi" 0;
createNode shadingEngine -n "lambert2SG";
	rename -uid "06F9F513-4F9C-2743-E2D5-0BAE8C061A60";
	setAttr ".ihi" 0;
	setAttr -s 2 ".dsm";
	setAttr ".ro" yes;
	setAttr -s 2 ".gn";
createNode materialInfo -n "materialInfo1";
	rename -uid "04F00416-4B5B-4002-C184-A19A94A0843A";
createNode lambert -n "lambert2";
	rename -uid "11E4AAB1-4184-C1CE-D438-1B9F80A2F594";
	setAttr ".c" -type "float3" 0.17175573 0.17175573 0.17175573 ;
createNode skinCluster -n "buddy_eye_l_skin";
	rename -uid "50CDA033-4D7A-5486-BC87-E296FFA0FE38";
	setAttr -s 382 ".wl";
	setAttr ".wl[0:381].w"
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1;
	setAttr ".pm[0]" -type "matrix" 0.96592582628906842 0 0.25881904510252079 0 0 1 0 0
		 -0.25881904510252079 0 0.96592582628906842 0 -4.9018532609177994 -38.173999999999999 -11.108816731246833 1;
	setAttr ".gm" -type "matrix" 0.96592582628906831 0 -0.25881904510252074 0 0 1 0 0
		 0.25881904510252074 0 0.96592582628906831 0 -1.3322676295501878e-15 0 -1.7763568394002505e-15 1;
	setAttr ".dpf[0]"  4;
	setAttr ".mi" 1;
	setAttr ".bm" 1;
	setAttr ".ucm" yes;
createNode groupParts -n "buddy_eye_l_skinGroupParts";
	rename -uid "A2A113DC-4F95-FBA5-849F-8A88E7A3D0E0";
	setAttr ".ihi" 0;
	setAttr ".ic" -type "componentList" 2 "f[300:359]" "f[380:399]";
createNode groupParts -n "groupParts1";
	rename -uid "C1C6BBF6-498F-7F52-F0E7-B18B2C125760";
	setAttr ".ihi" 0;
	setAttr ".ic" -type "componentList" 2 "f[0:299]" "f[360:379]";
createNode groupId -n "groupId20";
	rename -uid "F1A1E1A5-472F-C6E1-21B3-DAA31563235B";
	setAttr ".ihi" 0;
createNode groupId -n "groupId21";
	rename -uid "4CF4D7F2-4B3F-F347-CA4C-4EBD17A3FA82";
	setAttr ".ihi" 0;
createNode skinCluster -n "buddy_eye_r_skin";
	rename -uid "4E411CE2-4549-B3BE-5D47-B19E3F4A44C0";
	setAttr -s 382 ".wl";
	setAttr ".wl[0:381].w"
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1
		1 0 1;
	setAttr ".pm[0]" -type "matrix" 0.96592582628906798 2.4321330479367201e-16 -0.25881904510252068 0
		 -4.9769936625757152e-17 0.99999999999999978 2.4321330479367201e-16 0 0.25881904510252085 -4.9769936625757152e-17 0.96592582628906798 0
		 4.9018532609177985 -38.173999999999978 -11.108816731246838 1;
	setAttr ".gm" -type "matrix" 0.96592582628906831 0 0.25881904510252074 0 0 1 0 0
		 -0.25881904510252074 0 0.96592582628906831 0 4.4408920985006262e-16 0 0 1;
	setAttr ".dpf[0]"  4;
	setAttr ".mi" 1;
	setAttr ".bm" 1;
	setAttr ".ucm" yes;
createNode groupParts -n "buddy_eye_r_skinGroupParts";
	rename -uid "CD7559A2-4B58-7B6D-CD2D-58816B857895";
	setAttr ".ihi" 0;
	setAttr ".ic" -type "componentList" 2 "f[300:359]" "f[380:399]";
createNode groupParts -n "groupParts2";
	rename -uid "1189769B-4596-77EC-BD69-15BF9C216C4A";
	setAttr ".ihi" 0;
	setAttr ".ic" -type "componentList" 2 "f[0:299]" "f[360:379]";
createNode lightLinker -s -n "lightLinker1";
	rename -uid "3D20D5A9-49CE-CF03-C333-57BEC37F6E85";
	setAttr -s 6 ".lnk";
	setAttr -s 6 ".slnk";
connectAttr "main_CTL.globalScale" "main_CTL.sx" -l on;
connectAttr "main_CTL.globalScale" "main_CTL.sy" -l on;
connectAttr "main_CTL.globalScale" "main_CTL.sz" -l on;
connectAttr "skeleton_LYR.di" "skeleton_GRP.do";
connectAttr "root_parentConstraint.ctx" "root.tx";
connectAttr "root_parentConstraint.cty" "root.ty";
connectAttr "root_parentConstraint.ctz" "root.tz";
connectAttr "root_parentConstraint.crx" "root.rx";
connectAttr "root_parentConstraint.cry" "root.ry";
connectAttr "root_parentConstraint.crz" "root.rz";
connectAttr "root.s" "hips.is";
connectAttr "hips_parentConstraint.ctx" "hips.tx";
connectAttr "hips_parentConstraint.cty" "hips.ty";
connectAttr "hips_parentConstraint.ctz" "hips.tz";
connectAttr "hips_parentConstraint.crx" "hips.rx";
connectAttr "hips_parentConstraint.cry" "hips.ry";
connectAttr "hips_parentConstraint.crz" "hips.rz";
connectAttr "hips.s" "spine.is";
connectAttr "spine_parentConstraint.ctx" "spine.tx";
connectAttr "spine_parentConstraint.cty" "spine.ty";
connectAttr "spine_parentConstraint.ctz" "spine.tz";
connectAttr "spine_parentConstraint.crx" "spine.rx";
connectAttr "spine_parentConstraint.cry" "spine.ry";
connectAttr "spine_parentConstraint.crz" "spine.rz";
connectAttr "spine.s" "head.is";
connectAttr "head_parentConstraint.ctx" "head.tx";
connectAttr "head_parentConstraint.cty" "head.ty";
connectAttr "head_parentConstraint.ctz" "head.tz";
connectAttr "head_parentConstraint.crx" "head.rx";
connectAttr "head_parentConstraint.cry" "head.ry";
connectAttr "head_parentConstraint.crz" "head.rz";
connectAttr "head.s" "headEnd.is";
connectAttr "head.s" "upShoulder_L.is";
connectAttr "upShoulder_L_parentConstraint.ctx" "upShoulder_L.tx";
connectAttr "upShoulder_L_parentConstraint.cty" "upShoulder_L.ty";
connectAttr "upShoulder_L_parentConstraint.ctz" "upShoulder_L.tz";
connectAttr "upShoulder_L_parentConstraint.crx" "upShoulder_L.rx";
connectAttr "upShoulder_L_parentConstraint.cry" "upShoulder_L.ry";
connectAttr "upShoulder_L_parentConstraint.crz" "upShoulder_L.rz";
connectAttr "upShoulder_L.s" "upElbow_L.is";
connectAttr "upElbow_L_parentConstraint.ctx" "upElbow_L.tx";
connectAttr "upElbow_L_parentConstraint.cty" "upElbow_L.ty";
connectAttr "upElbow_L_parentConstraint.ctz" "upElbow_L.tz";
connectAttr "upElbow_L_parentConstraint.crx" "upElbow_L.rx";
connectAttr "upElbow_L_parentConstraint.cry" "upElbow_L.ry";
connectAttr "upElbow_L_parentConstraint.crz" "upElbow_L.rz";
connectAttr "upElbow_L.s" "upWrist_L.is";
connectAttr "upWrist_L_parentConstraint.ctx" "upWrist_L.tx";
connectAttr "upWrist_L_parentConstraint.cty" "upWrist_L.ty";
connectAttr "upWrist_L_parentConstraint.ctz" "upWrist_L.tz";
connectAttr "upWrist_L_parentConstraint.crx" "upWrist_L.rx";
connectAttr "upWrist_L_parentConstraint.cry" "upWrist_L.ry";
connectAttr "upWrist_L_parentConstraint.crz" "upWrist_L.rz";
connectAttr "upWrist_L.s" "upThumb1_L.is";
connectAttr "upThumb1_L_parentConstraint.ctx" "upThumb1_L.tx";
connectAttr "upThumb1_L_parentConstraint.cty" "upThumb1_L.ty";
connectAttr "upThumb1_L_parentConstraint.ctz" "upThumb1_L.tz";
connectAttr "upThumb1_L_parentConstraint.crx" "upThumb1_L.rx";
connectAttr "upThumb1_L_parentConstraint.cry" "upThumb1_L.ry";
connectAttr "upThumb1_L_parentConstraint.crz" "upThumb1_L.rz";
connectAttr "upThumb1_L.s" "upThumb2_L.is";
connectAttr "upThumb2_L_parentConstraint.ctx" "upThumb2_L.tx";
connectAttr "upThumb2_L_parentConstraint.cty" "upThumb2_L.ty";
connectAttr "upThumb2_L_parentConstraint.ctz" "upThumb2_L.tz";
connectAttr "upThumb2_L_parentConstraint.crx" "upThumb2_L.rx";
connectAttr "upThumb2_L_parentConstraint.cry" "upThumb2_L.ry";
connectAttr "upThumb2_L_parentConstraint.crz" "upThumb2_L.rz";
connectAttr "upThumb2_L.s" "upThumbEnd_L.is";
connectAttr "upThumb2_L.ro" "upThumb2_L_parentConstraint.cro";
connectAttr "upThumb2_L.pim" "upThumb2_L_parentConstraint.cpim";
connectAttr "upThumb2_L.rp" "upThumb2_L_parentConstraint.crp";
connectAttr "upThumb2_L.rpt" "upThumb2_L_parentConstraint.crt";
connectAttr "upThumb2_L.jo" "upThumb2_L_parentConstraint.cjo";
connectAttr "upThumb2_L_CTL.t" "upThumb2_L_parentConstraint.tg[0].tt";
connectAttr "upThumb2_L_CTL.rp" "upThumb2_L_parentConstraint.tg[0].trp";
connectAttr "upThumb2_L_CTL.rpt" "upThumb2_L_parentConstraint.tg[0].trt";
connectAttr "upThumb2_L_CTL.r" "upThumb2_L_parentConstraint.tg[0].tr";
connectAttr "upThumb2_L_CTL.ro" "upThumb2_L_parentConstraint.tg[0].tro";
connectAttr "upThumb2_L_CTL.s" "upThumb2_L_parentConstraint.tg[0].ts";
connectAttr "upThumb2_L_CTL.pm" "upThumb2_L_parentConstraint.tg[0].tpm";
connectAttr "upThumb2_L_parentConstraint.w0" "upThumb2_L_parentConstraint.tg[0].tw"
		;
connectAttr "upThumb1_L.ro" "upThumb1_L_parentConstraint.cro";
connectAttr "upThumb1_L.pim" "upThumb1_L_parentConstraint.cpim";
connectAttr "upThumb1_L.rp" "upThumb1_L_parentConstraint.crp";
connectAttr "upThumb1_L.rpt" "upThumb1_L_parentConstraint.crt";
connectAttr "upThumb1_L.jo" "upThumb1_L_parentConstraint.cjo";
connectAttr "upThumb1_L_CTL.t" "upThumb1_L_parentConstraint.tg[0].tt";
connectAttr "upThumb1_L_CTL.rp" "upThumb1_L_parentConstraint.tg[0].trp";
connectAttr "upThumb1_L_CTL.rpt" "upThumb1_L_parentConstraint.tg[0].trt";
connectAttr "upThumb1_L_CTL.r" "upThumb1_L_parentConstraint.tg[0].tr";
connectAttr "upThumb1_L_CTL.ro" "upThumb1_L_parentConstraint.tg[0].tro";
connectAttr "upThumb1_L_CTL.s" "upThumb1_L_parentConstraint.tg[0].ts";
connectAttr "upThumb1_L_CTL.pm" "upThumb1_L_parentConstraint.tg[0].tpm";
connectAttr "upThumb1_L_parentConstraint.w0" "upThumb1_L_parentConstraint.tg[0].tw"
		;
connectAttr "upWrist_L.s" "upFingerA1_L.is";
connectAttr "upFingerA1_L_parentConstraint.ctx" "upFingerA1_L.tx";
connectAttr "upFingerA1_L_parentConstraint.cty" "upFingerA1_L.ty";
connectAttr "upFingerA1_L_parentConstraint.ctz" "upFingerA1_L.tz";
connectAttr "upFingerA1_L_parentConstraint.crx" "upFingerA1_L.rx";
connectAttr "upFingerA1_L_parentConstraint.cry" "upFingerA1_L.ry";
connectAttr "upFingerA1_L_parentConstraint.crz" "upFingerA1_L.rz";
connectAttr "upFingerA1_L.s" "upFingerA2_L.is";
connectAttr "upFingerA2_L_parentConstraint.ctx" "upFingerA2_L.tx";
connectAttr "upFingerA2_L_parentConstraint.cty" "upFingerA2_L.ty";
connectAttr "upFingerA2_L_parentConstraint.ctz" "upFingerA2_L.tz";
connectAttr "upFingerA2_L_parentConstraint.crx" "upFingerA2_L.rx";
connectAttr "upFingerA2_L_parentConstraint.cry" "upFingerA2_L.ry";
connectAttr "upFingerA2_L_parentConstraint.crz" "upFingerA2_L.rz";
connectAttr "upFingerA2_L.s" "upFingerAEnd_L.is";
connectAttr "upFingerA2_L.ro" "upFingerA2_L_parentConstraint.cro";
connectAttr "upFingerA2_L.pim" "upFingerA2_L_parentConstraint.cpim";
connectAttr "upFingerA2_L.rp" "upFingerA2_L_parentConstraint.crp";
connectAttr "upFingerA2_L.rpt" "upFingerA2_L_parentConstraint.crt";
connectAttr "upFingerA2_L.jo" "upFingerA2_L_parentConstraint.cjo";
connectAttr "upFingerA2_L_CTL.t" "upFingerA2_L_parentConstraint.tg[0].tt";
connectAttr "upFingerA2_L_CTL.rp" "upFingerA2_L_parentConstraint.tg[0].trp";
connectAttr "upFingerA2_L_CTL.rpt" "upFingerA2_L_parentConstraint.tg[0].trt";
connectAttr "upFingerA2_L_CTL.r" "upFingerA2_L_parentConstraint.tg[0].tr";
connectAttr "upFingerA2_L_CTL.ro" "upFingerA2_L_parentConstraint.tg[0].tro";
connectAttr "upFingerA2_L_CTL.s" "upFingerA2_L_parentConstraint.tg[0].ts";
connectAttr "upFingerA2_L_CTL.pm" "upFingerA2_L_parentConstraint.tg[0].tpm";
connectAttr "upFingerA2_L_parentConstraint.w0" "upFingerA2_L_parentConstraint.tg[0].tw"
		;
connectAttr "upFingerA1_L.ro" "upFingerA1_L_parentConstraint.cro";
connectAttr "upFingerA1_L.pim" "upFingerA1_L_parentConstraint.cpim";
connectAttr "upFingerA1_L.rp" "upFingerA1_L_parentConstraint.crp";
connectAttr "upFingerA1_L.rpt" "upFingerA1_L_parentConstraint.crt";
connectAttr "upFingerA1_L.jo" "upFingerA1_L_parentConstraint.cjo";
connectAttr "upFingerA1_L_CTL.t" "upFingerA1_L_parentConstraint.tg[0].tt";
connectAttr "upFingerA1_L_CTL.rp" "upFingerA1_L_parentConstraint.tg[0].trp";
connectAttr "upFingerA1_L_CTL.rpt" "upFingerA1_L_parentConstraint.tg[0].trt";
connectAttr "upFingerA1_L_CTL.r" "upFingerA1_L_parentConstraint.tg[0].tr";
connectAttr "upFingerA1_L_CTL.ro" "upFingerA1_L_parentConstraint.tg[0].tro";
connectAttr "upFingerA1_L_CTL.s" "upFingerA1_L_parentConstraint.tg[0].ts";
connectAttr "upFingerA1_L_CTL.pm" "upFingerA1_L_parentConstraint.tg[0].tpm";
connectAttr "upFingerA1_L_parentConstraint.w0" "upFingerA1_L_parentConstraint.tg[0].tw"
		;
connectAttr "upWrist_L.s" "upFingerB1_L.is";
connectAttr "upFingerB1_L_parentConstraint.ctx" "upFingerB1_L.tx";
connectAttr "upFingerB1_L_parentConstraint.cty" "upFingerB1_L.ty";
connectAttr "upFingerB1_L_parentConstraint.ctz" "upFingerB1_L.tz";
connectAttr "upFingerB1_L_parentConstraint.crx" "upFingerB1_L.rx";
connectAttr "upFingerB1_L_parentConstraint.cry" "upFingerB1_L.ry";
connectAttr "upFingerB1_L_parentConstraint.crz" "upFingerB1_L.rz";
connectAttr "upFingerB1_L.s" "upFingerB2_L.is";
connectAttr "upFingerB2_L_parentConstraint.ctx" "upFingerB2_L.tx";
connectAttr "upFingerB2_L_parentConstraint.cty" "upFingerB2_L.ty";
connectAttr "upFingerB2_L_parentConstraint.ctz" "upFingerB2_L.tz";
connectAttr "upFingerB2_L_parentConstraint.crx" "upFingerB2_L.rx";
connectAttr "upFingerB2_L_parentConstraint.cry" "upFingerB2_L.ry";
connectAttr "upFingerB2_L_parentConstraint.crz" "upFingerB2_L.rz";
connectAttr "upFingerB2_L.s" "upFingerBEnd_L.is";
connectAttr "upFingerB2_L.ro" "upFingerB2_L_parentConstraint.cro";
connectAttr "upFingerB2_L.pim" "upFingerB2_L_parentConstraint.cpim";
connectAttr "upFingerB2_L.rp" "upFingerB2_L_parentConstraint.crp";
connectAttr "upFingerB2_L.rpt" "upFingerB2_L_parentConstraint.crt";
connectAttr "upFingerB2_L.jo" "upFingerB2_L_parentConstraint.cjo";
connectAttr "upFingerB2_L_CTL.t" "upFingerB2_L_parentConstraint.tg[0].tt";
connectAttr "upFingerB2_L_CTL.rp" "upFingerB2_L_parentConstraint.tg[0].trp";
connectAttr "upFingerB2_L_CTL.rpt" "upFingerB2_L_parentConstraint.tg[0].trt";
connectAttr "upFingerB2_L_CTL.r" "upFingerB2_L_parentConstraint.tg[0].tr";
connectAttr "upFingerB2_L_CTL.ro" "upFingerB2_L_parentConstraint.tg[0].tro";
connectAttr "upFingerB2_L_CTL.s" "upFingerB2_L_parentConstraint.tg[0].ts";
connectAttr "upFingerB2_L_CTL.pm" "upFingerB2_L_parentConstraint.tg[0].tpm";
connectAttr "upFingerB2_L_parentConstraint.w0" "upFingerB2_L_parentConstraint.tg[0].tw"
		;
connectAttr "upFingerB1_L.ro" "upFingerB1_L_parentConstraint.cro";
connectAttr "upFingerB1_L.pim" "upFingerB1_L_parentConstraint.cpim";
connectAttr "upFingerB1_L.rp" "upFingerB1_L_parentConstraint.crp";
connectAttr "upFingerB1_L.rpt" "upFingerB1_L_parentConstraint.crt";
connectAttr "upFingerB1_L.jo" "upFingerB1_L_parentConstraint.cjo";
connectAttr "upFingerB1_L_CTL.t" "upFingerB1_L_parentConstraint.tg[0].tt";
connectAttr "upFingerB1_L_CTL.rp" "upFingerB1_L_parentConstraint.tg[0].trp";
connectAttr "upFingerB1_L_CTL.rpt" "upFingerB1_L_parentConstraint.tg[0].trt";
connectAttr "upFingerB1_L_CTL.r" "upFingerB1_L_parentConstraint.tg[0].tr";
connectAttr "upFingerB1_L_CTL.ro" "upFingerB1_L_parentConstraint.tg[0].tro";
connectAttr "upFingerB1_L_CTL.s" "upFingerB1_L_parentConstraint.tg[0].ts";
connectAttr "upFingerB1_L_CTL.pm" "upFingerB1_L_parentConstraint.tg[0].tpm";
connectAttr "upFingerB1_L_parentConstraint.w0" "upFingerB1_L_parentConstraint.tg[0].tw"
		;
connectAttr "upWrist_L.ro" "upWrist_L_parentConstraint.cro";
connectAttr "upWrist_L.pim" "upWrist_L_parentConstraint.cpim";
connectAttr "upWrist_L.rp" "upWrist_L_parentConstraint.crp";
connectAttr "upWrist_L.rpt" "upWrist_L_parentConstraint.crt";
connectAttr "upWrist_L.jo" "upWrist_L_parentConstraint.cjo";
connectAttr "upWrist_L_CTL.t" "upWrist_L_parentConstraint.tg[0].tt";
connectAttr "upWrist_L_CTL.rp" "upWrist_L_parentConstraint.tg[0].trp";
connectAttr "upWrist_L_CTL.rpt" "upWrist_L_parentConstraint.tg[0].trt";
connectAttr "upWrist_L_CTL.r" "upWrist_L_parentConstraint.tg[0].tr";
connectAttr "upWrist_L_CTL.ro" "upWrist_L_parentConstraint.tg[0].tro";
connectAttr "upWrist_L_CTL.s" "upWrist_L_parentConstraint.tg[0].ts";
connectAttr "upWrist_L_CTL.pm" "upWrist_L_parentConstraint.tg[0].tpm";
connectAttr "upWrist_L_parentConstraint.w0" "upWrist_L_parentConstraint.tg[0].tw"
		;
connectAttr "upElbow_L.ro" "upElbow_L_parentConstraint.cro";
connectAttr "upElbow_L.pim" "upElbow_L_parentConstraint.cpim";
connectAttr "upElbow_L.rp" "upElbow_L_parentConstraint.crp";
connectAttr "upElbow_L.rpt" "upElbow_L_parentConstraint.crt";
connectAttr "upElbow_L.jo" "upElbow_L_parentConstraint.cjo";
connectAttr "upElbow_L_CTL.t" "upElbow_L_parentConstraint.tg[0].tt";
connectAttr "upElbow_L_CTL.rp" "upElbow_L_parentConstraint.tg[0].trp";
connectAttr "upElbow_L_CTL.rpt" "upElbow_L_parentConstraint.tg[0].trt";
connectAttr "upElbow_L_CTL.r" "upElbow_L_parentConstraint.tg[0].tr";
connectAttr "upElbow_L_CTL.ro" "upElbow_L_parentConstraint.tg[0].tro";
connectAttr "upElbow_L_CTL.s" "upElbow_L_parentConstraint.tg[0].ts";
connectAttr "upElbow_L_CTL.pm" "upElbow_L_parentConstraint.tg[0].tpm";
connectAttr "upElbow_L_parentConstraint.w0" "upElbow_L_parentConstraint.tg[0].tw"
		;
connectAttr "upShoulder_L.ro" "upShoulder_L_parentConstraint.cro";
connectAttr "upShoulder_L.pim" "upShoulder_L_parentConstraint.cpim";
connectAttr "upShoulder_L.rp" "upShoulder_L_parentConstraint.crp";
connectAttr "upShoulder_L.rpt" "upShoulder_L_parentConstraint.crt";
connectAttr "upShoulder_L.jo" "upShoulder_L_parentConstraint.cjo";
connectAttr "upShoulder_L_CTL.t" "upShoulder_L_parentConstraint.tg[0].tt";
connectAttr "upShoulder_L_CTL.rp" "upShoulder_L_parentConstraint.tg[0].trp";
connectAttr "upShoulder_L_CTL.rpt" "upShoulder_L_parentConstraint.tg[0].trt";
connectAttr "upShoulder_L_CTL.r" "upShoulder_L_parentConstraint.tg[0].tr";
connectAttr "upShoulder_L_CTL.ro" "upShoulder_L_parentConstraint.tg[0].tro";
connectAttr "upShoulder_L_CTL.s" "upShoulder_L_parentConstraint.tg[0].ts";
connectAttr "upShoulder_L_CTL.pm" "upShoulder_L_parentConstraint.tg[0].tpm";
connectAttr "upShoulder_L_parentConstraint.w0" "upShoulder_L_parentConstraint.tg[0].tw"
		;
connectAttr "head.s" "upShoulder_R.is";
connectAttr "upShoulder_R_parentConstraint.ctx" "upShoulder_R.tx";
connectAttr "upShoulder_R_parentConstraint.cty" "upShoulder_R.ty";
connectAttr "upShoulder_R_parentConstraint.ctz" "upShoulder_R.tz";
connectAttr "upShoulder_R_parentConstraint.crx" "upShoulder_R.rx";
connectAttr "upShoulder_R_parentConstraint.cry" "upShoulder_R.ry";
connectAttr "upShoulder_R_parentConstraint.crz" "upShoulder_R.rz";
connectAttr "upShoulder_R.s" "upElbow_R.is";
connectAttr "upElbow_R_parentConstraint.ctx" "upElbow_R.tx";
connectAttr "upElbow_R_parentConstraint.cty" "upElbow_R.ty";
connectAttr "upElbow_R_parentConstraint.ctz" "upElbow_R.tz";
connectAttr "upElbow_R_parentConstraint.crx" "upElbow_R.rx";
connectAttr "upElbow_R_parentConstraint.cry" "upElbow_R.ry";
connectAttr "upElbow_R_parentConstraint.crz" "upElbow_R.rz";
connectAttr "upElbow_R.s" "upWrist_R.is";
connectAttr "upWrist_R_parentConstraint.ctx" "upWrist_R.tx";
connectAttr "upWrist_R_parentConstraint.cty" "upWrist_R.ty";
connectAttr "upWrist_R_parentConstraint.ctz" "upWrist_R.tz";
connectAttr "upWrist_R_parentConstraint.crx" "upWrist_R.rx";
connectAttr "upWrist_R_parentConstraint.cry" "upWrist_R.ry";
connectAttr "upWrist_R_parentConstraint.crz" "upWrist_R.rz";
connectAttr "upWrist_R.s" "upThumb1_R.is";
connectAttr "upThumb1_R_parentConstraint.ctx" "upThumb1_R.tx";
connectAttr "upThumb1_R_parentConstraint.cty" "upThumb1_R.ty";
connectAttr "upThumb1_R_parentConstraint.ctz" "upThumb1_R.tz";
connectAttr "upThumb1_R_parentConstraint.crx" "upThumb1_R.rx";
connectAttr "upThumb1_R_parentConstraint.cry" "upThumb1_R.ry";
connectAttr "upThumb1_R_parentConstraint.crz" "upThumb1_R.rz";
connectAttr "upThumb1_R.s" "upThumb2_R.is";
connectAttr "upThumb2_R_parentConstraint.ctx" "upThumb2_R.tx";
connectAttr "upThumb2_R_parentConstraint.cty" "upThumb2_R.ty";
connectAttr "upThumb2_R_parentConstraint.ctz" "upThumb2_R.tz";
connectAttr "upThumb2_R_parentConstraint.crx" "upThumb2_R.rx";
connectAttr "upThumb2_R_parentConstraint.cry" "upThumb2_R.ry";
connectAttr "upThumb2_R_parentConstraint.crz" "upThumb2_R.rz";
connectAttr "upThumb2_R.s" "upThumbEnd_R.is";
connectAttr "upThumb2_R.ro" "upThumb2_R_parentConstraint.cro";
connectAttr "upThumb2_R.pim" "upThumb2_R_parentConstraint.cpim";
connectAttr "upThumb2_R.rp" "upThumb2_R_parentConstraint.crp";
connectAttr "upThumb2_R.rpt" "upThumb2_R_parentConstraint.crt";
connectAttr "upThumb2_R.jo" "upThumb2_R_parentConstraint.cjo";
connectAttr "upThumb2_R_CTL.t" "upThumb2_R_parentConstraint.tg[0].tt";
connectAttr "upThumb2_R_CTL.rp" "upThumb2_R_parentConstraint.tg[0].trp";
connectAttr "upThumb2_R_CTL.rpt" "upThumb2_R_parentConstraint.tg[0].trt";
connectAttr "upThumb2_R_CTL.r" "upThumb2_R_parentConstraint.tg[0].tr";
connectAttr "upThumb2_R_CTL.ro" "upThumb2_R_parentConstraint.tg[0].tro";
connectAttr "upThumb2_R_CTL.s" "upThumb2_R_parentConstraint.tg[0].ts";
connectAttr "upThumb2_R_CTL.pm" "upThumb2_R_parentConstraint.tg[0].tpm";
connectAttr "upThumb2_R_parentConstraint.w0" "upThumb2_R_parentConstraint.tg[0].tw"
		;
connectAttr "upThumb1_R.ro" "upThumb1_R_parentConstraint.cro";
connectAttr "upThumb1_R.pim" "upThumb1_R_parentConstraint.cpim";
connectAttr "upThumb1_R.rp" "upThumb1_R_parentConstraint.crp";
connectAttr "upThumb1_R.rpt" "upThumb1_R_parentConstraint.crt";
connectAttr "upThumb1_R.jo" "upThumb1_R_parentConstraint.cjo";
connectAttr "upThumb1_R_CTL.t" "upThumb1_R_parentConstraint.tg[0].tt";
connectAttr "upThumb1_R_CTL.rp" "upThumb1_R_parentConstraint.tg[0].trp";
connectAttr "upThumb1_R_CTL.rpt" "upThumb1_R_parentConstraint.tg[0].trt";
connectAttr "upThumb1_R_CTL.r" "upThumb1_R_parentConstraint.tg[0].tr";
connectAttr "upThumb1_R_CTL.ro" "upThumb1_R_parentConstraint.tg[0].tro";
connectAttr "upThumb1_R_CTL.s" "upThumb1_R_parentConstraint.tg[0].ts";
connectAttr "upThumb1_R_CTL.pm" "upThumb1_R_parentConstraint.tg[0].tpm";
connectAttr "upThumb1_R_parentConstraint.w0" "upThumb1_R_parentConstraint.tg[0].tw"
		;
connectAttr "upWrist_R.s" "upFingerA1_R.is";
connectAttr "upFingerA1_R_parentConstraint.ctx" "upFingerA1_R.tx";
connectAttr "upFingerA1_R_parentConstraint.cty" "upFingerA1_R.ty";
connectAttr "upFingerA1_R_parentConstraint.ctz" "upFingerA1_R.tz";
connectAttr "upFingerA1_R_parentConstraint.crx" "upFingerA1_R.rx";
connectAttr "upFingerA1_R_parentConstraint.cry" "upFingerA1_R.ry";
connectAttr "upFingerA1_R_parentConstraint.crz" "upFingerA1_R.rz";
connectAttr "upFingerA1_R.s" "upFingerA2_R.is";
connectAttr "upFingerA2_R_parentConstraint.ctx" "upFingerA2_R.tx";
connectAttr "upFingerA2_R_parentConstraint.cty" "upFingerA2_R.ty";
connectAttr "upFingerA2_R_parentConstraint.ctz" "upFingerA2_R.tz";
connectAttr "upFingerA2_R_parentConstraint.crx" "upFingerA2_R.rx";
connectAttr "upFingerA2_R_parentConstraint.cry" "upFingerA2_R.ry";
connectAttr "upFingerA2_R_parentConstraint.crz" "upFingerA2_R.rz";
connectAttr "upFingerA2_R.s" "upFingerAEnd_R.is";
connectAttr "upFingerA2_R.ro" "upFingerA2_R_parentConstraint.cro";
connectAttr "upFingerA2_R.pim" "upFingerA2_R_parentConstraint.cpim";
connectAttr "upFingerA2_R.rp" "upFingerA2_R_parentConstraint.crp";
connectAttr "upFingerA2_R.rpt" "upFingerA2_R_parentConstraint.crt";
connectAttr "upFingerA2_R.jo" "upFingerA2_R_parentConstraint.cjo";
connectAttr "upFingerA2_R_CTL.t" "upFingerA2_R_parentConstraint.tg[0].tt";
connectAttr "upFingerA2_R_CTL.rp" "upFingerA2_R_parentConstraint.tg[0].trp";
connectAttr "upFingerA2_R_CTL.rpt" "upFingerA2_R_parentConstraint.tg[0].trt";
connectAttr "upFingerA2_R_CTL.r" "upFingerA2_R_parentConstraint.tg[0].tr";
connectAttr "upFingerA2_R_CTL.ro" "upFingerA2_R_parentConstraint.tg[0].tro";
connectAttr "upFingerA2_R_CTL.s" "upFingerA2_R_parentConstraint.tg[0].ts";
connectAttr "upFingerA2_R_CTL.pm" "upFingerA2_R_parentConstraint.tg[0].tpm";
connectAttr "upFingerA2_R_parentConstraint.w0" "upFingerA2_R_parentConstraint.tg[0].tw"
		;
connectAttr "upFingerA1_R.ro" "upFingerA1_R_parentConstraint.cro";
connectAttr "upFingerA1_R.pim" "upFingerA1_R_parentConstraint.cpim";
connectAttr "upFingerA1_R.rp" "upFingerA1_R_parentConstraint.crp";
connectAttr "upFingerA1_R.rpt" "upFingerA1_R_parentConstraint.crt";
connectAttr "upFingerA1_R.jo" "upFingerA1_R_parentConstraint.cjo";
connectAttr "upFingerA1_R_CTL.t" "upFingerA1_R_parentConstraint.tg[0].tt";
connectAttr "upFingerA1_R_CTL.rp" "upFingerA1_R_parentConstraint.tg[0].trp";
connectAttr "upFingerA1_R_CTL.rpt" "upFingerA1_R_parentConstraint.tg[0].trt";
connectAttr "upFingerA1_R_CTL.r" "upFingerA1_R_parentConstraint.tg[0].tr";
connectAttr "upFingerA1_R_CTL.ro" "upFingerA1_R_parentConstraint.tg[0].tro";
connectAttr "upFingerA1_R_CTL.s" "upFingerA1_R_parentConstraint.tg[0].ts";
connectAttr "upFingerA1_R_CTL.pm" "upFingerA1_R_parentConstraint.tg[0].tpm";
connectAttr "upFingerA1_R_parentConstraint.w0" "upFingerA1_R_parentConstraint.tg[0].tw"
		;
connectAttr "upWrist_R.s" "upFingerB1_R.is";
connectAttr "upFingerB1_R_parentConstraint.ctx" "upFingerB1_R.tx";
connectAttr "upFingerB1_R_parentConstraint.cty" "upFingerB1_R.ty";
connectAttr "upFingerB1_R_parentConstraint.ctz" "upFingerB1_R.tz";
connectAttr "upFingerB1_R_parentConstraint.crx" "upFingerB1_R.rx";
connectAttr "upFingerB1_R_parentConstraint.cry" "upFingerB1_R.ry";
connectAttr "upFingerB1_R_parentConstraint.crz" "upFingerB1_R.rz";
connectAttr "upFingerB1_R.s" "upFingerB2_R.is";
connectAttr "upFingerB2_R_parentConstraint.ctx" "upFingerB2_R.tx";
connectAttr "upFingerB2_R_parentConstraint.cty" "upFingerB2_R.ty";
connectAttr "upFingerB2_R_parentConstraint.ctz" "upFingerB2_R.tz";
connectAttr "upFingerB2_R_parentConstraint.crx" "upFingerB2_R.rx";
connectAttr "upFingerB2_R_parentConstraint.cry" "upFingerB2_R.ry";
connectAttr "upFingerB2_R_parentConstraint.crz" "upFingerB2_R.rz";
connectAttr "upFingerB2_R.s" "upFingerBEnd_R.is";
connectAttr "upFingerB2_R.ro" "upFingerB2_R_parentConstraint.cro";
connectAttr "upFingerB2_R.pim" "upFingerB2_R_parentConstraint.cpim";
connectAttr "upFingerB2_R.rp" "upFingerB2_R_parentConstraint.crp";
connectAttr "upFingerB2_R.rpt" "upFingerB2_R_parentConstraint.crt";
connectAttr "upFingerB2_R.jo" "upFingerB2_R_parentConstraint.cjo";
connectAttr "upFingerB2_R_CTL.t" "upFingerB2_R_parentConstraint.tg[0].tt";
connectAttr "upFingerB2_R_CTL.rp" "upFingerB2_R_parentConstraint.tg[0].trp";
connectAttr "upFingerB2_R_CTL.rpt" "upFingerB2_R_parentConstraint.tg[0].trt";
connectAttr "upFingerB2_R_CTL.r" "upFingerB2_R_parentConstraint.tg[0].tr";
connectAttr "upFingerB2_R_CTL.ro" "upFingerB2_R_parentConstraint.tg[0].tro";
connectAttr "upFingerB2_R_CTL.s" "upFingerB2_R_parentConstraint.tg[0].ts";
connectAttr "upFingerB2_R_CTL.pm" "upFingerB2_R_parentConstraint.tg[0].tpm";
connectAttr "upFingerB2_R_parentConstraint.w0" "upFingerB2_R_parentConstraint.tg[0].tw"
		;
connectAttr "upFingerB1_R.ro" "upFingerB1_R_parentConstraint.cro";
connectAttr "upFingerB1_R.pim" "upFingerB1_R_parentConstraint.cpim";
connectAttr "upFingerB1_R.rp" "upFingerB1_R_parentConstraint.crp";
connectAttr "upFingerB1_R.rpt" "upFingerB1_R_parentConstraint.crt";
connectAttr "upFingerB1_R.jo" "upFingerB1_R_parentConstraint.cjo";
connectAttr "upFingerB1_R_CTL.t" "upFingerB1_R_parentConstraint.tg[0].tt";
connectAttr "upFingerB1_R_CTL.rp" "upFingerB1_R_parentConstraint.tg[0].trp";
connectAttr "upFingerB1_R_CTL.rpt" "upFingerB1_R_parentConstraint.tg[0].trt";
connectAttr "upFingerB1_R_CTL.r" "upFingerB1_R_parentConstraint.tg[0].tr";
connectAttr "upFingerB1_R_CTL.ro" "upFingerB1_R_parentConstraint.tg[0].tro";
connectAttr "upFingerB1_R_CTL.s" "upFingerB1_R_parentConstraint.tg[0].ts";
connectAttr "upFingerB1_R_CTL.pm" "upFingerB1_R_parentConstraint.tg[0].tpm";
connectAttr "upFingerB1_R_parentConstraint.w0" "upFingerB1_R_parentConstraint.tg[0].tw"
		;
connectAttr "upWrist_R.ro" "upWrist_R_parentConstraint.cro";
connectAttr "upWrist_R.pim" "upWrist_R_parentConstraint.cpim";
connectAttr "upWrist_R.rp" "upWrist_R_parentConstraint.crp";
connectAttr "upWrist_R.rpt" "upWrist_R_parentConstraint.crt";
connectAttr "upWrist_R.jo" "upWrist_R_parentConstraint.cjo";
connectAttr "upWrist_R_CTL.t" "upWrist_R_parentConstraint.tg[0].tt";
connectAttr "upWrist_R_CTL.rp" "upWrist_R_parentConstraint.tg[0].trp";
connectAttr "upWrist_R_CTL.rpt" "upWrist_R_parentConstraint.tg[0].trt";
connectAttr "upWrist_R_CTL.r" "upWrist_R_parentConstraint.tg[0].tr";
connectAttr "upWrist_R_CTL.ro" "upWrist_R_parentConstraint.tg[0].tro";
connectAttr "upWrist_R_CTL.s" "upWrist_R_parentConstraint.tg[0].ts";
connectAttr "upWrist_R_CTL.pm" "upWrist_R_parentConstraint.tg[0].tpm";
connectAttr "upWrist_R_parentConstraint.w0" "upWrist_R_parentConstraint.tg[0].tw"
		;
connectAttr "upElbow_R.ro" "upElbow_R_parentConstraint.cro";
connectAttr "upElbow_R.pim" "upElbow_R_parentConstraint.cpim";
connectAttr "upElbow_R.rp" "upElbow_R_parentConstraint.crp";
connectAttr "upElbow_R.rpt" "upElbow_R_parentConstraint.crt";
connectAttr "upElbow_R.jo" "upElbow_R_parentConstraint.cjo";
connectAttr "upElbow_R_CTL.t" "upElbow_R_parentConstraint.tg[0].tt";
connectAttr "upElbow_R_CTL.rp" "upElbow_R_parentConstraint.tg[0].trp";
connectAttr "upElbow_R_CTL.rpt" "upElbow_R_parentConstraint.tg[0].trt";
connectAttr "upElbow_R_CTL.r" "upElbow_R_parentConstraint.tg[0].tr";
connectAttr "upElbow_R_CTL.ro" "upElbow_R_parentConstraint.tg[0].tro";
connectAttr "upElbow_R_CTL.s" "upElbow_R_parentConstraint.tg[0].ts";
connectAttr "upElbow_R_CTL.pm" "upElbow_R_parentConstraint.tg[0].tpm";
connectAttr "upElbow_R_parentConstraint.w0" "upElbow_R_parentConstraint.tg[0].tw"
		;
connectAttr "upShoulder_R.ro" "upShoulder_R_parentConstraint.cro";
connectAttr "upShoulder_R.pim" "upShoulder_R_parentConstraint.cpim";
connectAttr "upShoulder_R.rp" "upShoulder_R_parentConstraint.crp";
connectAttr "upShoulder_R.rpt" "upShoulder_R_parentConstraint.crt";
connectAttr "upShoulder_R.jo" "upShoulder_R_parentConstraint.cjo";
connectAttr "upShoulder_R_CTL.t" "upShoulder_R_parentConstraint.tg[0].tt";
connectAttr "upShoulder_R_CTL.rp" "upShoulder_R_parentConstraint.tg[0].trp";
connectAttr "upShoulder_R_CTL.rpt" "upShoulder_R_parentConstraint.tg[0].trt";
connectAttr "upShoulder_R_CTL.r" "upShoulder_R_parentConstraint.tg[0].tr";
connectAttr "upShoulder_R_CTL.ro" "upShoulder_R_parentConstraint.tg[0].tro";
connectAttr "upShoulder_R_CTL.s" "upShoulder_R_parentConstraint.tg[0].ts";
connectAttr "upShoulder_R_CTL.pm" "upShoulder_R_parentConstraint.tg[0].tpm";
connectAttr "upShoulder_R_parentConstraint.w0" "upShoulder_R_parentConstraint.tg[0].tw"
		;
connectAttr "head.s" "eye_L.is";
connectAttr "eye_L_aimConstraint.crx" "eye_L.rx";
connectAttr "eye_L_aimConstraint.cry" "eye_L.ry";
connectAttr "eye_L_aimConstraint.crz" "eye_L.rz";
connectAttr "eye_L.pim" "eye_L_aimConstraint.cpim";
connectAttr "eye_L.t" "eye_L_aimConstraint.ct";
connectAttr "eye_L.rp" "eye_L_aimConstraint.crp";
connectAttr "eye_L.rpt" "eye_L_aimConstraint.crt";
connectAttr "eye_L.ro" "eye_L_aimConstraint.cro";
connectAttr "eye_L.jo" "eye_L_aimConstraint.cjo";
connectAttr "eye_L.is" "eye_L_aimConstraint.is";
connectAttr "eye_L_CTL.t" "eye_L_aimConstraint.tg[0].tt";
connectAttr "eye_L_CTL.rp" "eye_L_aimConstraint.tg[0].trp";
connectAttr "eye_L_CTL.rpt" "eye_L_aimConstraint.tg[0].trt";
connectAttr "eye_L_CTL.pm" "eye_L_aimConstraint.tg[0].tpm";
connectAttr "eye_L_aimConstraint.w0" "eye_L_aimConstraint.tg[0].tw";
connectAttr "eyes_CTL.wm" "eye_L_aimConstraint.wum";
connectAttr "head.s" "eye_R.is";
connectAttr "eye_R_aimConstraint.crx" "eye_R.rx";
connectAttr "eye_R_aimConstraint.cry" "eye_R.ry";
connectAttr "eye_R_aimConstraint.crz" "eye_R.rz";
connectAttr "eye_R.pim" "eye_R_aimConstraint.cpim";
connectAttr "eye_R.t" "eye_R_aimConstraint.ct";
connectAttr "eye_R.rp" "eye_R_aimConstraint.crp";
connectAttr "eye_R.rpt" "eye_R_aimConstraint.crt";
connectAttr "eye_R.ro" "eye_R_aimConstraint.cro";
connectAttr "eye_R.jo" "eye_R_aimConstraint.cjo";
connectAttr "eye_R.is" "eye_R_aimConstraint.is";
connectAttr "eye_R_CTL.t" "eye_R_aimConstraint.tg[0].tt";
connectAttr "eye_R_CTL.rp" "eye_R_aimConstraint.tg[0].trp";
connectAttr "eye_R_CTL.rpt" "eye_R_aimConstraint.tg[0].trt";
connectAttr "eye_R_CTL.pm" "eye_R_aimConstraint.tg[0].tpm";
connectAttr "eye_R_aimConstraint.w0" "eye_R_aimConstraint.tg[0].tw";
connectAttr "eyes_CTL.wm" "eye_R_aimConstraint.wum";
connectAttr "head.ro" "head_parentConstraint.cro";
connectAttr "head.pim" "head_parentConstraint.cpim";
connectAttr "head.rp" "head_parentConstraint.crp";
connectAttr "head.rpt" "head_parentConstraint.crt";
connectAttr "head.jo" "head_parentConstraint.cjo";
connectAttr "head_CTL.t" "head_parentConstraint.tg[0].tt";
connectAttr "head_CTL.rp" "head_parentConstraint.tg[0].trp";
connectAttr "head_CTL.rpt" "head_parentConstraint.tg[0].trt";
connectAttr "head_CTL.r" "head_parentConstraint.tg[0].tr";
connectAttr "head_CTL.ro" "head_parentConstraint.tg[0].tro";
connectAttr "head_CTL.s" "head_parentConstraint.tg[0].ts";
connectAttr "head_CTL.pm" "head_parentConstraint.tg[0].tpm";
connectAttr "head_parentConstraint.w0" "head_parentConstraint.tg[0].tw";
connectAttr "spine.s" "loShoulder_L.is";
connectAttr "loShoulder_L_parentConstraint.ctx" "loShoulder_L.tx";
connectAttr "loShoulder_L_parentConstraint.cty" "loShoulder_L.ty";
connectAttr "loShoulder_L_parentConstraint.ctz" "loShoulder_L.tz";
connectAttr "loShoulder_L_parentConstraint.crx" "loShoulder_L.rx";
connectAttr "loShoulder_L_parentConstraint.cry" "loShoulder_L.ry";
connectAttr "loShoulder_L_parentConstraint.crz" "loShoulder_L.rz";
connectAttr "loShoulder_L.s" "loElbow_L.is";
connectAttr "loElbow_L_parentConstraint.ctx" "loElbow_L.tx";
connectAttr "loElbow_L_parentConstraint.cty" "loElbow_L.ty";
connectAttr "loElbow_L_parentConstraint.ctz" "loElbow_L.tz";
connectAttr "loElbow_L_parentConstraint.crx" "loElbow_L.rx";
connectAttr "loElbow_L_parentConstraint.cry" "loElbow_L.ry";
connectAttr "loElbow_L_parentConstraint.crz" "loElbow_L.rz";
connectAttr "loElbow_L.s" "loWrist_L.is";
connectAttr "loWrist_L_parentConstraint.ctx" "loWrist_L.tx";
connectAttr "loWrist_L_parentConstraint.cty" "loWrist_L.ty";
connectAttr "loWrist_L_parentConstraint.ctz" "loWrist_L.tz";
connectAttr "loWrist_L_parentConstraint.crx" "loWrist_L.rx";
connectAttr "loWrist_L_parentConstraint.cry" "loWrist_L.ry";
connectAttr "loWrist_L_parentConstraint.crz" "loWrist_L.rz";
connectAttr "loWrist_L.s" "loThumb1_L.is";
connectAttr "loThumb1_L_parentConstraint.ctx" "loThumb1_L.tx";
connectAttr "loThumb1_L_parentConstraint.cty" "loThumb1_L.ty";
connectAttr "loThumb1_L_parentConstraint.ctz" "loThumb1_L.tz";
connectAttr "loThumb1_L_parentConstraint.crx" "loThumb1_L.rx";
connectAttr "loThumb1_L_parentConstraint.cry" "loThumb1_L.ry";
connectAttr "loThumb1_L_parentConstraint.crz" "loThumb1_L.rz";
connectAttr "loThumb1_L.s" "loThumb2_L.is";
connectAttr "loThumb2_L_parentConstraint.ctx" "loThumb2_L.tx";
connectAttr "loThumb2_L_parentConstraint.cty" "loThumb2_L.ty";
connectAttr "loThumb2_L_parentConstraint.ctz" "loThumb2_L.tz";
connectAttr "loThumb2_L_parentConstraint.crx" "loThumb2_L.rx";
connectAttr "loThumb2_L_parentConstraint.cry" "loThumb2_L.ry";
connectAttr "loThumb2_L_parentConstraint.crz" "loThumb2_L.rz";
connectAttr "loThumb2_L.s" "loThumbEnd_L.is";
connectAttr "loThumb2_L.ro" "loThumb2_L_parentConstraint.cro";
connectAttr "loThumb2_L.pim" "loThumb2_L_parentConstraint.cpim";
connectAttr "loThumb2_L.rp" "loThumb2_L_parentConstraint.crp";
connectAttr "loThumb2_L.rpt" "loThumb2_L_parentConstraint.crt";
connectAttr "loThumb2_L.jo" "loThumb2_L_parentConstraint.cjo";
connectAttr "loThumb2_L_CTL.t" "loThumb2_L_parentConstraint.tg[0].tt";
connectAttr "loThumb2_L_CTL.rp" "loThumb2_L_parentConstraint.tg[0].trp";
connectAttr "loThumb2_L_CTL.rpt" "loThumb2_L_parentConstraint.tg[0].trt";
connectAttr "loThumb2_L_CTL.r" "loThumb2_L_parentConstraint.tg[0].tr";
connectAttr "loThumb2_L_CTL.ro" "loThumb2_L_parentConstraint.tg[0].tro";
connectAttr "loThumb2_L_CTL.s" "loThumb2_L_parentConstraint.tg[0].ts";
connectAttr "loThumb2_L_CTL.pm" "loThumb2_L_parentConstraint.tg[0].tpm";
connectAttr "loThumb2_L_parentConstraint.w0" "loThumb2_L_parentConstraint.tg[0].tw"
		;
connectAttr "loThumb1_L.ro" "loThumb1_L_parentConstraint.cro";
connectAttr "loThumb1_L.pim" "loThumb1_L_parentConstraint.cpim";
connectAttr "loThumb1_L.rp" "loThumb1_L_parentConstraint.crp";
connectAttr "loThumb1_L.rpt" "loThumb1_L_parentConstraint.crt";
connectAttr "loThumb1_L.jo" "loThumb1_L_parentConstraint.cjo";
connectAttr "loThumb1_L_CTL.t" "loThumb1_L_parentConstraint.tg[0].tt";
connectAttr "loThumb1_L_CTL.rp" "loThumb1_L_parentConstraint.tg[0].trp";
connectAttr "loThumb1_L_CTL.rpt" "loThumb1_L_parentConstraint.tg[0].trt";
connectAttr "loThumb1_L_CTL.r" "loThumb1_L_parentConstraint.tg[0].tr";
connectAttr "loThumb1_L_CTL.ro" "loThumb1_L_parentConstraint.tg[0].tro";
connectAttr "loThumb1_L_CTL.s" "loThumb1_L_parentConstraint.tg[0].ts";
connectAttr "loThumb1_L_CTL.pm" "loThumb1_L_parentConstraint.tg[0].tpm";
connectAttr "loThumb1_L_parentConstraint.w0" "loThumb1_L_parentConstraint.tg[0].tw"
		;
connectAttr "loWrist_L.s" "loFingerA1_L.is";
connectAttr "loFingerA1_L_parentConstraint.ctx" "loFingerA1_L.tx";
connectAttr "loFingerA1_L_parentConstraint.cty" "loFingerA1_L.ty";
connectAttr "loFingerA1_L_parentConstraint.ctz" "loFingerA1_L.tz";
connectAttr "loFingerA1_L_parentConstraint.crx" "loFingerA1_L.rx";
connectAttr "loFingerA1_L_parentConstraint.cry" "loFingerA1_L.ry";
connectAttr "loFingerA1_L_parentConstraint.crz" "loFingerA1_L.rz";
connectAttr "loFingerA1_L.s" "loFingerA2_L.is";
connectAttr "loFingerA2_L_parentConstraint.ctx" "loFingerA2_L.tx";
connectAttr "loFingerA2_L_parentConstraint.cty" "loFingerA2_L.ty";
connectAttr "loFingerA2_L_parentConstraint.ctz" "loFingerA2_L.tz";
connectAttr "loFingerA2_L_parentConstraint.crx" "loFingerA2_L.rx";
connectAttr "loFingerA2_L_parentConstraint.cry" "loFingerA2_L.ry";
connectAttr "loFingerA2_L_parentConstraint.crz" "loFingerA2_L.rz";
connectAttr "loFingerA2_L.s" "loFingerAEnd_L.is";
connectAttr "loFingerA2_L.ro" "loFingerA2_L_parentConstraint.cro";
connectAttr "loFingerA2_L.pim" "loFingerA2_L_parentConstraint.cpim";
connectAttr "loFingerA2_L.rp" "loFingerA2_L_parentConstraint.crp";
connectAttr "loFingerA2_L.rpt" "loFingerA2_L_parentConstraint.crt";
connectAttr "loFingerA2_L.jo" "loFingerA2_L_parentConstraint.cjo";
connectAttr "loFingerA2_L_CTL.t" "loFingerA2_L_parentConstraint.tg[0].tt";
connectAttr "loFingerA2_L_CTL.rp" "loFingerA2_L_parentConstraint.tg[0].trp";
connectAttr "loFingerA2_L_CTL.rpt" "loFingerA2_L_parentConstraint.tg[0].trt";
connectAttr "loFingerA2_L_CTL.r" "loFingerA2_L_parentConstraint.tg[0].tr";
connectAttr "loFingerA2_L_CTL.ro" "loFingerA2_L_parentConstraint.tg[0].tro";
connectAttr "loFingerA2_L_CTL.s" "loFingerA2_L_parentConstraint.tg[0].ts";
connectAttr "loFingerA2_L_CTL.pm" "loFingerA2_L_parentConstraint.tg[0].tpm";
connectAttr "loFingerA2_L_parentConstraint.w0" "loFingerA2_L_parentConstraint.tg[0].tw"
		;
connectAttr "loFingerA1_L.ro" "loFingerA1_L_parentConstraint.cro";
connectAttr "loFingerA1_L.pim" "loFingerA1_L_parentConstraint.cpim";
connectAttr "loFingerA1_L.rp" "loFingerA1_L_parentConstraint.crp";
connectAttr "loFingerA1_L.rpt" "loFingerA1_L_parentConstraint.crt";
connectAttr "loFingerA1_L.jo" "loFingerA1_L_parentConstraint.cjo";
connectAttr "loFingerA1_L_CTL.t" "loFingerA1_L_parentConstraint.tg[0].tt";
connectAttr "loFingerA1_L_CTL.rp" "loFingerA1_L_parentConstraint.tg[0].trp";
connectAttr "loFingerA1_L_CTL.rpt" "loFingerA1_L_parentConstraint.tg[0].trt";
connectAttr "loFingerA1_L_CTL.r" "loFingerA1_L_parentConstraint.tg[0].tr";
connectAttr "loFingerA1_L_CTL.ro" "loFingerA1_L_parentConstraint.tg[0].tro";
connectAttr "loFingerA1_L_CTL.s" "loFingerA1_L_parentConstraint.tg[0].ts";
connectAttr "loFingerA1_L_CTL.pm" "loFingerA1_L_parentConstraint.tg[0].tpm";
connectAttr "loFingerA1_L_parentConstraint.w0" "loFingerA1_L_parentConstraint.tg[0].tw"
		;
connectAttr "loWrist_L.s" "loFingerB1_L.is";
connectAttr "loFingerB1_L_parentConstraint.ctx" "loFingerB1_L.tx";
connectAttr "loFingerB1_L_parentConstraint.cty" "loFingerB1_L.ty";
connectAttr "loFingerB1_L_parentConstraint.ctz" "loFingerB1_L.tz";
connectAttr "loFingerB1_L_parentConstraint.crx" "loFingerB1_L.rx";
connectAttr "loFingerB1_L_parentConstraint.cry" "loFingerB1_L.ry";
connectAttr "loFingerB1_L_parentConstraint.crz" "loFingerB1_L.rz";
connectAttr "loFingerB1_L.s" "loFingerB2_L.is";
connectAttr "loFingerB2_L_parentConstraint.ctx" "loFingerB2_L.tx";
connectAttr "loFingerB2_L_parentConstraint.cty" "loFingerB2_L.ty";
connectAttr "loFingerB2_L_parentConstraint.ctz" "loFingerB2_L.tz";
connectAttr "loFingerB2_L_parentConstraint.crx" "loFingerB2_L.rx";
connectAttr "loFingerB2_L_parentConstraint.cry" "loFingerB2_L.ry";
connectAttr "loFingerB2_L_parentConstraint.crz" "loFingerB2_L.rz";
connectAttr "loFingerB2_L.s" "loFingerBEnd_L.is";
connectAttr "loFingerB2_L.ro" "loFingerB2_L_parentConstraint.cro";
connectAttr "loFingerB2_L.pim" "loFingerB2_L_parentConstraint.cpim";
connectAttr "loFingerB2_L.rp" "loFingerB2_L_parentConstraint.crp";
connectAttr "loFingerB2_L.rpt" "loFingerB2_L_parentConstraint.crt";
connectAttr "loFingerB2_L.jo" "loFingerB2_L_parentConstraint.cjo";
connectAttr "loFingerB2_L_CTL.t" "loFingerB2_L_parentConstraint.tg[0].tt";
connectAttr "loFingerB2_L_CTL.rp" "loFingerB2_L_parentConstraint.tg[0].trp";
connectAttr "loFingerB2_L_CTL.rpt" "loFingerB2_L_parentConstraint.tg[0].trt";
connectAttr "loFingerB2_L_CTL.r" "loFingerB2_L_parentConstraint.tg[0].tr";
connectAttr "loFingerB2_L_CTL.ro" "loFingerB2_L_parentConstraint.tg[0].tro";
connectAttr "loFingerB2_L_CTL.s" "loFingerB2_L_parentConstraint.tg[0].ts";
connectAttr "loFingerB2_L_CTL.pm" "loFingerB2_L_parentConstraint.tg[0].tpm";
connectAttr "loFingerB2_L_parentConstraint.w0" "loFingerB2_L_parentConstraint.tg[0].tw"
		;
connectAttr "loFingerB1_L.ro" "loFingerB1_L_parentConstraint.cro";
connectAttr "loFingerB1_L.pim" "loFingerB1_L_parentConstraint.cpim";
connectAttr "loFingerB1_L.rp" "loFingerB1_L_parentConstraint.crp";
connectAttr "loFingerB1_L.rpt" "loFingerB1_L_parentConstraint.crt";
connectAttr "loFingerB1_L.jo" "loFingerB1_L_parentConstraint.cjo";
connectAttr "loFingerB1_L_CTL.t" "loFingerB1_L_parentConstraint.tg[0].tt";
connectAttr "loFingerB1_L_CTL.rp" "loFingerB1_L_parentConstraint.tg[0].trp";
connectAttr "loFingerB1_L_CTL.rpt" "loFingerB1_L_parentConstraint.tg[0].trt";
connectAttr "loFingerB1_L_CTL.r" "loFingerB1_L_parentConstraint.tg[0].tr";
connectAttr "loFingerB1_L_CTL.ro" "loFingerB1_L_parentConstraint.tg[0].tro";
connectAttr "loFingerB1_L_CTL.s" "loFingerB1_L_parentConstraint.tg[0].ts";
connectAttr "loFingerB1_L_CTL.pm" "loFingerB1_L_parentConstraint.tg[0].tpm";
connectAttr "loFingerB1_L_parentConstraint.w0" "loFingerB1_L_parentConstraint.tg[0].tw"
		;
connectAttr "loWrist_L.ro" "loWrist_L_parentConstraint.cro";
connectAttr "loWrist_L.pim" "loWrist_L_parentConstraint.cpim";
connectAttr "loWrist_L.rp" "loWrist_L_parentConstraint.crp";
connectAttr "loWrist_L.rpt" "loWrist_L_parentConstraint.crt";
connectAttr "loWrist_L.jo" "loWrist_L_parentConstraint.cjo";
connectAttr "loWrist_L_CTL.t" "loWrist_L_parentConstraint.tg[0].tt";
connectAttr "loWrist_L_CTL.rp" "loWrist_L_parentConstraint.tg[0].trp";
connectAttr "loWrist_L_CTL.rpt" "loWrist_L_parentConstraint.tg[0].trt";
connectAttr "loWrist_L_CTL.r" "loWrist_L_parentConstraint.tg[0].tr";
connectAttr "loWrist_L_CTL.ro" "loWrist_L_parentConstraint.tg[0].tro";
connectAttr "loWrist_L_CTL.s" "loWrist_L_parentConstraint.tg[0].ts";
connectAttr "loWrist_L_CTL.pm" "loWrist_L_parentConstraint.tg[0].tpm";
connectAttr "loWrist_L_parentConstraint.w0" "loWrist_L_parentConstraint.tg[0].tw"
		;
connectAttr "loElbow_L.ro" "loElbow_L_parentConstraint.cro";
connectAttr "loElbow_L.pim" "loElbow_L_parentConstraint.cpim";
connectAttr "loElbow_L.rp" "loElbow_L_parentConstraint.crp";
connectAttr "loElbow_L.rpt" "loElbow_L_parentConstraint.crt";
connectAttr "loElbow_L.jo" "loElbow_L_parentConstraint.cjo";
connectAttr "loElbow_L_CTL.t" "loElbow_L_parentConstraint.tg[0].tt";
connectAttr "loElbow_L_CTL.rp" "loElbow_L_parentConstraint.tg[0].trp";
connectAttr "loElbow_L_CTL.rpt" "loElbow_L_parentConstraint.tg[0].trt";
connectAttr "loElbow_L_CTL.r" "loElbow_L_parentConstraint.tg[0].tr";
connectAttr "loElbow_L_CTL.ro" "loElbow_L_parentConstraint.tg[0].tro";
connectAttr "loElbow_L_CTL.s" "loElbow_L_parentConstraint.tg[0].ts";
connectAttr "loElbow_L_CTL.pm" "loElbow_L_parentConstraint.tg[0].tpm";
connectAttr "loElbow_L_parentConstraint.w0" "loElbow_L_parentConstraint.tg[0].tw"
		;
connectAttr "loShoulder_L.ro" "loShoulder_L_parentConstraint.cro";
connectAttr "loShoulder_L.pim" "loShoulder_L_parentConstraint.cpim";
connectAttr "loShoulder_L.rp" "loShoulder_L_parentConstraint.crp";
connectAttr "loShoulder_L.rpt" "loShoulder_L_parentConstraint.crt";
connectAttr "loShoulder_L.jo" "loShoulder_L_parentConstraint.cjo";
connectAttr "loShoulder_L_CTL.t" "loShoulder_L_parentConstraint.tg[0].tt";
connectAttr "loShoulder_L_CTL.rp" "loShoulder_L_parentConstraint.tg[0].trp";
connectAttr "loShoulder_L_CTL.rpt" "loShoulder_L_parentConstraint.tg[0].trt";
connectAttr "loShoulder_L_CTL.r" "loShoulder_L_parentConstraint.tg[0].tr";
connectAttr "loShoulder_L_CTL.ro" "loShoulder_L_parentConstraint.tg[0].tro";
connectAttr "loShoulder_L_CTL.s" "loShoulder_L_parentConstraint.tg[0].ts";
connectAttr "loShoulder_L_CTL.pm" "loShoulder_L_parentConstraint.tg[0].tpm";
connectAttr "loShoulder_L_parentConstraint.w0" "loShoulder_L_parentConstraint.tg[0].tw"
		;
connectAttr "spine.s" "loShoulder_R.is";
connectAttr "loShoulder_R_parentConstraint.ctx" "loShoulder_R.tx";
connectAttr "loShoulder_R_parentConstraint.cty" "loShoulder_R.ty";
connectAttr "loShoulder_R_parentConstraint.ctz" "loShoulder_R.tz";
connectAttr "loShoulder_R_parentConstraint.crx" "loShoulder_R.rx";
connectAttr "loShoulder_R_parentConstraint.cry" "loShoulder_R.ry";
connectAttr "loShoulder_R_parentConstraint.crz" "loShoulder_R.rz";
connectAttr "loShoulder_R.s" "loElbow_R.is";
connectAttr "loElbow_R_parentConstraint.ctx" "loElbow_R.tx";
connectAttr "loElbow_R_parentConstraint.cty" "loElbow_R.ty";
connectAttr "loElbow_R_parentConstraint.ctz" "loElbow_R.tz";
connectAttr "loElbow_R_parentConstraint.crx" "loElbow_R.rx";
connectAttr "loElbow_R_parentConstraint.cry" "loElbow_R.ry";
connectAttr "loElbow_R_parentConstraint.crz" "loElbow_R.rz";
connectAttr "loElbow_R.s" "loWrist_R.is";
connectAttr "loWrist_R_parentConstraint.ctx" "loWrist_R.tx";
connectAttr "loWrist_R_parentConstraint.cty" "loWrist_R.ty";
connectAttr "loWrist_R_parentConstraint.ctz" "loWrist_R.tz";
connectAttr "loWrist_R_parentConstraint.crx" "loWrist_R.rx";
connectAttr "loWrist_R_parentConstraint.cry" "loWrist_R.ry";
connectAttr "loWrist_R_parentConstraint.crz" "loWrist_R.rz";
connectAttr "loWrist_R.s" "loThumb1_R.is";
connectAttr "loThumb1_R_parentConstraint.ctx" "loThumb1_R.tx";
connectAttr "loThumb1_R_parentConstraint.cty" "loThumb1_R.ty";
connectAttr "loThumb1_R_parentConstraint.ctz" "loThumb1_R.tz";
connectAttr "loThumb1_R_parentConstraint.crx" "loThumb1_R.rx";
connectAttr "loThumb1_R_parentConstraint.cry" "loThumb1_R.ry";
connectAttr "loThumb1_R_parentConstraint.crz" "loThumb1_R.rz";
connectAttr "loThumb1_R.s" "loThumb2_R.is";
connectAttr "loThumb2_R_parentConstraint.ctx" "loThumb2_R.tx";
connectAttr "loThumb2_R_parentConstraint.cty" "loThumb2_R.ty";
connectAttr "loThumb2_R_parentConstraint.ctz" "loThumb2_R.tz";
connectAttr "loThumb2_R_parentConstraint.crx" "loThumb2_R.rx";
connectAttr "loThumb2_R_parentConstraint.cry" "loThumb2_R.ry";
connectAttr "loThumb2_R_parentConstraint.crz" "loThumb2_R.rz";
connectAttr "loThumb2_R.s" "loThumbEnd_R.is";
connectAttr "loThumb2_R.ro" "loThumb2_R_parentConstraint.cro";
connectAttr "loThumb2_R.pim" "loThumb2_R_parentConstraint.cpim";
connectAttr "loThumb2_R.rp" "loThumb2_R_parentConstraint.crp";
connectAttr "loThumb2_R.rpt" "loThumb2_R_parentConstraint.crt";
connectAttr "loThumb2_R.jo" "loThumb2_R_parentConstraint.cjo";
connectAttr "loThumb2_R_CTL.t" "loThumb2_R_parentConstraint.tg[0].tt";
connectAttr "loThumb2_R_CTL.rp" "loThumb2_R_parentConstraint.tg[0].trp";
connectAttr "loThumb2_R_CTL.rpt" "loThumb2_R_parentConstraint.tg[0].trt";
connectAttr "loThumb2_R_CTL.r" "loThumb2_R_parentConstraint.tg[0].tr";
connectAttr "loThumb2_R_CTL.ro" "loThumb2_R_parentConstraint.tg[0].tro";
connectAttr "loThumb2_R_CTL.s" "loThumb2_R_parentConstraint.tg[0].ts";
connectAttr "loThumb2_R_CTL.pm" "loThumb2_R_parentConstraint.tg[0].tpm";
connectAttr "loThumb2_R_parentConstraint.w0" "loThumb2_R_parentConstraint.tg[0].tw"
		;
connectAttr "loThumb1_R.ro" "loThumb1_R_parentConstraint.cro";
connectAttr "loThumb1_R.pim" "loThumb1_R_parentConstraint.cpim";
connectAttr "loThumb1_R.rp" "loThumb1_R_parentConstraint.crp";
connectAttr "loThumb1_R.rpt" "loThumb1_R_parentConstraint.crt";
connectAttr "loThumb1_R.jo" "loThumb1_R_parentConstraint.cjo";
connectAttr "loThumb1_R_CTL.t" "loThumb1_R_parentConstraint.tg[0].tt";
connectAttr "loThumb1_R_CTL.rp" "loThumb1_R_parentConstraint.tg[0].trp";
connectAttr "loThumb1_R_CTL.rpt" "loThumb1_R_parentConstraint.tg[0].trt";
connectAttr "loThumb1_R_CTL.r" "loThumb1_R_parentConstraint.tg[0].tr";
connectAttr "loThumb1_R_CTL.ro" "loThumb1_R_parentConstraint.tg[0].tro";
connectAttr "loThumb1_R_CTL.s" "loThumb1_R_parentConstraint.tg[0].ts";
connectAttr "loThumb1_R_CTL.pm" "loThumb1_R_parentConstraint.tg[0].tpm";
connectAttr "loThumb1_R_parentConstraint.w0" "loThumb1_R_parentConstraint.tg[0].tw"
		;
connectAttr "loWrist_R.s" "loFingerA1_R.is";
connectAttr "loFingerA1_R_parentConstraint.ctx" "loFingerA1_R.tx";
connectAttr "loFingerA1_R_parentConstraint.cty" "loFingerA1_R.ty";
connectAttr "loFingerA1_R_parentConstraint.ctz" "loFingerA1_R.tz";
connectAttr "loFingerA1_R_parentConstraint.crx" "loFingerA1_R.rx";
connectAttr "loFingerA1_R_parentConstraint.cry" "loFingerA1_R.ry";
connectAttr "loFingerA1_R_parentConstraint.crz" "loFingerA1_R.rz";
connectAttr "loFingerA1_R.s" "loFingerA2_R.is";
connectAttr "loFingerA2_R_parentConstraint.ctx" "loFingerA2_R.tx";
connectAttr "loFingerA2_R_parentConstraint.cty" "loFingerA2_R.ty";
connectAttr "loFingerA2_R_parentConstraint.ctz" "loFingerA2_R.tz";
connectAttr "loFingerA2_R_parentConstraint.crx" "loFingerA2_R.rx";
connectAttr "loFingerA2_R_parentConstraint.cry" "loFingerA2_R.ry";
connectAttr "loFingerA2_R_parentConstraint.crz" "loFingerA2_R.rz";
connectAttr "loFingerA2_R.s" "loFingerAEnd_R.is";
connectAttr "loFingerA2_R.ro" "loFingerA2_R_parentConstraint.cro";
connectAttr "loFingerA2_R.pim" "loFingerA2_R_parentConstraint.cpim";
connectAttr "loFingerA2_R.rp" "loFingerA2_R_parentConstraint.crp";
connectAttr "loFingerA2_R.rpt" "loFingerA2_R_parentConstraint.crt";
connectAttr "loFingerA2_R.jo" "loFingerA2_R_parentConstraint.cjo";
connectAttr "loFingerA2_R_CTL.t" "loFingerA2_R_parentConstraint.tg[0].tt";
connectAttr "loFingerA2_R_CTL.rp" "loFingerA2_R_parentConstraint.tg[0].trp";
connectAttr "loFingerA2_R_CTL.rpt" "loFingerA2_R_parentConstraint.tg[0].trt";
connectAttr "loFingerA2_R_CTL.r" "loFingerA2_R_parentConstraint.tg[0].tr";
connectAttr "loFingerA2_R_CTL.ro" "loFingerA2_R_parentConstraint.tg[0].tro";
connectAttr "loFingerA2_R_CTL.s" "loFingerA2_R_parentConstraint.tg[0].ts";
connectAttr "loFingerA2_R_CTL.pm" "loFingerA2_R_parentConstraint.tg[0].tpm";
connectAttr "loFingerA2_R_parentConstraint.w0" "loFingerA2_R_parentConstraint.tg[0].tw"
		;
connectAttr "loFingerA1_R.ro" "loFingerA1_R_parentConstraint.cro";
connectAttr "loFingerA1_R.pim" "loFingerA1_R_parentConstraint.cpim";
connectAttr "loFingerA1_R.rp" "loFingerA1_R_parentConstraint.crp";
connectAttr "loFingerA1_R.rpt" "loFingerA1_R_parentConstraint.crt";
connectAttr "loFingerA1_R.jo" "loFingerA1_R_parentConstraint.cjo";
connectAttr "loFingerA1_R_CTL.t" "loFingerA1_R_parentConstraint.tg[0].tt";
connectAttr "loFingerA1_R_CTL.rp" "loFingerA1_R_parentConstraint.tg[0].trp";
connectAttr "loFingerA1_R_CTL.rpt" "loFingerA1_R_parentConstraint.tg[0].trt";
connectAttr "loFingerA1_R_CTL.r" "loFingerA1_R_parentConstraint.tg[0].tr";
connectAttr "loFingerA1_R_CTL.ro" "loFingerA1_R_parentConstraint.tg[0].tro";
connectAttr "loFingerA1_R_CTL.s" "loFingerA1_R_parentConstraint.tg[0].ts";
connectAttr "loFingerA1_R_CTL.pm" "loFingerA1_R_parentConstraint.tg[0].tpm";
connectAttr "loFingerA1_R_parentConstraint.w0" "loFingerA1_R_parentConstraint.tg[0].tw"
		;
connectAttr "loWrist_R.s" "loFingerB1_R.is";
connectAttr "loFingerB1_R_parentConstraint.ctx" "loFingerB1_R.tx";
connectAttr "loFingerB1_R_parentConstraint.cty" "loFingerB1_R.ty";
connectAttr "loFingerB1_R_parentConstraint.ctz" "loFingerB1_R.tz";
connectAttr "loFingerB1_R_parentConstraint.crx" "loFingerB1_R.rx";
connectAttr "loFingerB1_R_parentConstraint.cry" "loFingerB1_R.ry";
connectAttr "loFingerB1_R_parentConstraint.crz" "loFingerB1_R.rz";
connectAttr "loFingerB1_R.s" "loFingerB2_R.is";
connectAttr "loFingerB2_R_parentConstraint.ctx" "loFingerB2_R.tx";
connectAttr "loFingerB2_R_parentConstraint.cty" "loFingerB2_R.ty";
connectAttr "loFingerB2_R_parentConstraint.ctz" "loFingerB2_R.tz";
connectAttr "loFingerB2_R_parentConstraint.crx" "loFingerB2_R.rx";
connectAttr "loFingerB2_R_parentConstraint.cry" "loFingerB2_R.ry";
connectAttr "loFingerB2_R_parentConstraint.crz" "loFingerB2_R.rz";
connectAttr "loFingerB2_R.s" "loFingerBEnd_R.is";
connectAttr "loFingerB2_R.ro" "loFingerB2_R_parentConstraint.cro";
connectAttr "loFingerB2_R.pim" "loFingerB2_R_parentConstraint.cpim";
connectAttr "loFingerB2_R.rp" "loFingerB2_R_parentConstraint.crp";
connectAttr "loFingerB2_R.rpt" "loFingerB2_R_parentConstraint.crt";
connectAttr "loFingerB2_R.jo" "loFingerB2_R_parentConstraint.cjo";
connectAttr "loFingerB2_R_CTL.t" "loFingerB2_R_parentConstraint.tg[0].tt";
connectAttr "loFingerB2_R_CTL.rp" "loFingerB2_R_parentConstraint.tg[0].trp";
connectAttr "loFingerB2_R_CTL.rpt" "loFingerB2_R_parentConstraint.tg[0].trt";
connectAttr "loFingerB2_R_CTL.r" "loFingerB2_R_parentConstraint.tg[0].tr";
connectAttr "loFingerB2_R_CTL.ro" "loFingerB2_R_parentConstraint.tg[0].tro";
connectAttr "loFingerB2_R_CTL.s" "loFingerB2_R_parentConstraint.tg[0].ts";
connectAttr "loFingerB2_R_CTL.pm" "loFingerB2_R_parentConstraint.tg[0].tpm";
connectAttr "loFingerB2_R_parentConstraint.w0" "loFingerB2_R_parentConstraint.tg[0].tw"
		;
connectAttr "loFingerB1_R.ro" "loFingerB1_R_parentConstraint.cro";
connectAttr "loFingerB1_R.pim" "loFingerB1_R_parentConstraint.cpim";
connectAttr "loFingerB1_R.rp" "loFingerB1_R_parentConstraint.crp";
connectAttr "loFingerB1_R.rpt" "loFingerB1_R_parentConstraint.crt";
connectAttr "loFingerB1_R.jo" "loFingerB1_R_parentConstraint.cjo";
connectAttr "loFingerB1_R_CTL.t" "loFingerB1_R_parentConstraint.tg[0].tt";
connectAttr "loFingerB1_R_CTL.rp" "loFingerB1_R_parentConstraint.tg[0].trp";
connectAttr "loFingerB1_R_CTL.rpt" "loFingerB1_R_parentConstraint.tg[0].trt";
connectAttr "loFingerB1_R_CTL.r" "loFingerB1_R_parentConstraint.tg[0].tr";
connectAttr "loFingerB1_R_CTL.ro" "loFingerB1_R_parentConstraint.tg[0].tro";
connectAttr "loFingerB1_R_CTL.s" "loFingerB1_R_parentConstraint.tg[0].ts";
connectAttr "loFingerB1_R_CTL.pm" "loFingerB1_R_parentConstraint.tg[0].tpm";
connectAttr "loFingerB1_R_parentConstraint.w0" "loFingerB1_R_parentConstraint.tg[0].tw"
		;
connectAttr "loWrist_R.ro" "loWrist_R_parentConstraint.cro";
connectAttr "loWrist_R.pim" "loWrist_R_parentConstraint.cpim";
connectAttr "loWrist_R.rp" "loWrist_R_parentConstraint.crp";
connectAttr "loWrist_R.rpt" "loWrist_R_parentConstraint.crt";
connectAttr "loWrist_R.jo" "loWrist_R_parentConstraint.cjo";
connectAttr "loWrist_R_CTL.t" "loWrist_R_parentConstraint.tg[0].tt";
connectAttr "loWrist_R_CTL.rp" "loWrist_R_parentConstraint.tg[0].trp";
connectAttr "loWrist_R_CTL.rpt" "loWrist_R_parentConstraint.tg[0].trt";
connectAttr "loWrist_R_CTL.r" "loWrist_R_parentConstraint.tg[0].tr";
connectAttr "loWrist_R_CTL.ro" "loWrist_R_parentConstraint.tg[0].tro";
connectAttr "loWrist_R_CTL.s" "loWrist_R_parentConstraint.tg[0].ts";
connectAttr "loWrist_R_CTL.pm" "loWrist_R_parentConstraint.tg[0].tpm";
connectAttr "loWrist_R_parentConstraint.w0" "loWrist_R_parentConstraint.tg[0].tw"
		;
connectAttr "loElbow_R.ro" "loElbow_R_parentConstraint.cro";
connectAttr "loElbow_R.pim" "loElbow_R_parentConstraint.cpim";
connectAttr "loElbow_R.rp" "loElbow_R_parentConstraint.crp";
connectAttr "loElbow_R.rpt" "loElbow_R_parentConstraint.crt";
connectAttr "loElbow_R.jo" "loElbow_R_parentConstraint.cjo";
connectAttr "loElbow_R_CTL.t" "loElbow_R_parentConstraint.tg[0].tt";
connectAttr "loElbow_R_CTL.rp" "loElbow_R_parentConstraint.tg[0].trp";
connectAttr "loElbow_R_CTL.rpt" "loElbow_R_parentConstraint.tg[0].trt";
connectAttr "loElbow_R_CTL.r" "loElbow_R_parentConstraint.tg[0].tr";
connectAttr "loElbow_R_CTL.ro" "loElbow_R_parentConstraint.tg[0].tro";
connectAttr "loElbow_R_CTL.s" "loElbow_R_parentConstraint.tg[0].ts";
connectAttr "loElbow_R_CTL.pm" "loElbow_R_parentConstraint.tg[0].tpm";
connectAttr "loElbow_R_parentConstraint.w0" "loElbow_R_parentConstraint.tg[0].tw"
		;
connectAttr "loShoulder_R.ro" "loShoulder_R_parentConstraint.cro";
connectAttr "loShoulder_R.pim" "loShoulder_R_parentConstraint.cpim";
connectAttr "loShoulder_R.rp" "loShoulder_R_parentConstraint.crp";
connectAttr "loShoulder_R.rpt" "loShoulder_R_parentConstraint.crt";
connectAttr "loShoulder_R.jo" "loShoulder_R_parentConstraint.cjo";
connectAttr "loShoulder_R_CTL.t" "loShoulder_R_parentConstraint.tg[0].tt";
connectAttr "loShoulder_R_CTL.rp" "loShoulder_R_parentConstraint.tg[0].trp";
connectAttr "loShoulder_R_CTL.rpt" "loShoulder_R_parentConstraint.tg[0].trt";
connectAttr "loShoulder_R_CTL.r" "loShoulder_R_parentConstraint.tg[0].tr";
connectAttr "loShoulder_R_CTL.ro" "loShoulder_R_parentConstraint.tg[0].tro";
connectAttr "loShoulder_R_CTL.s" "loShoulder_R_parentConstraint.tg[0].ts";
connectAttr "loShoulder_R_CTL.pm" "loShoulder_R_parentConstraint.tg[0].tpm";
connectAttr "loShoulder_R_parentConstraint.w0" "loShoulder_R_parentConstraint.tg[0].tw"
		;
connectAttr "spine.ro" "spine_parentConstraint.cro";
connectAttr "spine.pim" "spine_parentConstraint.cpim";
connectAttr "spine.rp" "spine_parentConstraint.crp";
connectAttr "spine.rpt" "spine_parentConstraint.crt";
connectAttr "spine.jo" "spine_parentConstraint.cjo";
connectAttr "spine_CTL.t" "spine_parentConstraint.tg[0].tt";
connectAttr "spine_CTL.rp" "spine_parentConstraint.tg[0].trp";
connectAttr "spine_CTL.rpt" "spine_parentConstraint.tg[0].trt";
connectAttr "spine_CTL.r" "spine_parentConstraint.tg[0].tr";
connectAttr "spine_CTL.ro" "spine_parentConstraint.tg[0].tro";
connectAttr "spine_CTL.s" "spine_parentConstraint.tg[0].ts";
connectAttr "spine_CTL.pm" "spine_parentConstraint.tg[0].tpm";
connectAttr "spine_parentConstraint.w0" "spine_parentConstraint.tg[0].tw";
connectAttr "hips.ro" "hips_parentConstraint.cro";
connectAttr "hips.pim" "hips_parentConstraint.cpim";
connectAttr "hips.rp" "hips_parentConstraint.crp";
connectAttr "hips.rpt" "hips_parentConstraint.crt";
connectAttr "hips.jo" "hips_parentConstraint.cjo";
connectAttr "hips_CTL.t" "hips_parentConstraint.tg[0].tt";
connectAttr "hips_CTL.rp" "hips_parentConstraint.tg[0].trp";
connectAttr "hips_CTL.rpt" "hips_parentConstraint.tg[0].trt";
connectAttr "hips_CTL.r" "hips_parentConstraint.tg[0].tr";
connectAttr "hips_CTL.ro" "hips_parentConstraint.tg[0].tro";
connectAttr "hips_CTL.s" "hips_parentConstraint.tg[0].ts";
connectAttr "hips_CTL.pm" "hips_parentConstraint.tg[0].tpm";
connectAttr "hips_parentConstraint.w0" "hips_parentConstraint.tg[0].tw";
connectAttr "root.s" "leg_L.is";
connectAttr "leg_L_parentConstraint.ctx" "leg_L.tx";
connectAttr "leg_L_parentConstraint.cty" "leg_L.ty";
connectAttr "leg_L_parentConstraint.ctz" "leg_L.tz";
connectAttr "leg_L_parentConstraint.crx" "leg_L.rx";
connectAttr "leg_L_parentConstraint.cry" "leg_L.ry";
connectAttr "leg_L_parentConstraint.crz" "leg_L.rz";
connectAttr "leg_L.s" "knee_L.is";
connectAttr "knee_L_parentConstraint.ctx" "knee_L.tx";
connectAttr "knee_L_parentConstraint.cty" "knee_L.ty";
connectAttr "knee_L_parentConstraint.ctz" "knee_L.tz";
connectAttr "knee_L_parentConstraint.crx" "knee_L.rx";
connectAttr "knee_L_parentConstraint.cry" "knee_L.ry";
connectAttr "knee_L_parentConstraint.crz" "knee_L.rz";
connectAttr "knee_L.s" "ankle_L.is";
connectAttr "ankle_L_parentConstraint.ctx" "ankle_L.tx";
connectAttr "ankle_L_parentConstraint.cty" "ankle_L.ty";
connectAttr "ankle_L_parentConstraint.ctz" "ankle_L.tz";
connectAttr "ankle_L_parentConstraint.crx" "ankle_L.rx";
connectAttr "ankle_L_parentConstraint.cry" "ankle_L.ry";
connectAttr "ankle_L_parentConstraint.crz" "ankle_L.rz";
connectAttr "ankle_L.s" "footEnd_L.is";
connectAttr "ankle_L.ro" "ankle_L_parentConstraint.cro";
connectAttr "ankle_L.pim" "ankle_L_parentConstraint.cpim";
connectAttr "ankle_L.rp" "ankle_L_parentConstraint.crp";
connectAttr "ankle_L.rpt" "ankle_L_parentConstraint.crt";
connectAttr "ankle_L.jo" "ankle_L_parentConstraint.cjo";
connectAttr "ankle_L_CTL.t" "ankle_L_parentConstraint.tg[0].tt";
connectAttr "ankle_L_CTL.rp" "ankle_L_parentConstraint.tg[0].trp";
connectAttr "ankle_L_CTL.rpt" "ankle_L_parentConstraint.tg[0].trt";
connectAttr "ankle_L_CTL.r" "ankle_L_parentConstraint.tg[0].tr";
connectAttr "ankle_L_CTL.ro" "ankle_L_parentConstraint.tg[0].tro";
connectAttr "ankle_L_CTL.s" "ankle_L_parentConstraint.tg[0].ts";
connectAttr "ankle_L_CTL.pm" "ankle_L_parentConstraint.tg[0].tpm";
connectAttr "ankle_L_parentConstraint.w0" "ankle_L_parentConstraint.tg[0].tw";
connectAttr "knee_L.ro" "knee_L_parentConstraint.cro";
connectAttr "knee_L.pim" "knee_L_parentConstraint.cpim";
connectAttr "knee_L.rp" "knee_L_parentConstraint.crp";
connectAttr "knee_L.rpt" "knee_L_parentConstraint.crt";
connectAttr "knee_L.jo" "knee_L_parentConstraint.cjo";
connectAttr "knee_L_CTL.t" "knee_L_parentConstraint.tg[0].tt";
connectAttr "knee_L_CTL.rp" "knee_L_parentConstraint.tg[0].trp";
connectAttr "knee_L_CTL.rpt" "knee_L_parentConstraint.tg[0].trt";
connectAttr "knee_L_CTL.r" "knee_L_parentConstraint.tg[0].tr";
connectAttr "knee_L_CTL.ro" "knee_L_parentConstraint.tg[0].tro";
connectAttr "knee_L_CTL.s" "knee_L_parentConstraint.tg[0].ts";
connectAttr "knee_L_CTL.pm" "knee_L_parentConstraint.tg[0].tpm";
connectAttr "knee_L_parentConstraint.w0" "knee_L_parentConstraint.tg[0].tw";
connectAttr "leg_L.ro" "leg_L_parentConstraint.cro";
connectAttr "leg_L.pim" "leg_L_parentConstraint.cpim";
connectAttr "leg_L.rp" "leg_L_parentConstraint.crp";
connectAttr "leg_L.rpt" "leg_L_parentConstraint.crt";
connectAttr "leg_L.jo" "leg_L_parentConstraint.cjo";
connectAttr "leg_L_CTL.t" "leg_L_parentConstraint.tg[0].tt";
connectAttr "leg_L_CTL.rp" "leg_L_parentConstraint.tg[0].trp";
connectAttr "leg_L_CTL.rpt" "leg_L_parentConstraint.tg[0].trt";
connectAttr "leg_L_CTL.r" "leg_L_parentConstraint.tg[0].tr";
connectAttr "leg_L_CTL.ro" "leg_L_parentConstraint.tg[0].tro";
connectAttr "leg_L_CTL.s" "leg_L_parentConstraint.tg[0].ts";
connectAttr "leg_L_CTL.pm" "leg_L_parentConstraint.tg[0].tpm";
connectAttr "leg_L_parentConstraint.w0" "leg_L_parentConstraint.tg[0].tw";
connectAttr "root.s" "leg_R.is";
connectAttr "leg_R_parentConstraint.ctx" "leg_R.tx";
connectAttr "leg_R_parentConstraint.cty" "leg_R.ty";
connectAttr "leg_R_parentConstraint.ctz" "leg_R.tz";
connectAttr "leg_R_parentConstraint.crx" "leg_R.rx";
connectAttr "leg_R_parentConstraint.cry" "leg_R.ry";
connectAttr "leg_R_parentConstraint.crz" "leg_R.rz";
connectAttr "leg_R.s" "knee_R.is";
connectAttr "knee_R_parentConstraint.ctx" "knee_R.tx";
connectAttr "knee_R_parentConstraint.cty" "knee_R.ty";
connectAttr "knee_R_parentConstraint.ctz" "knee_R.tz";
connectAttr "knee_R_parentConstraint.crx" "knee_R.rx";
connectAttr "knee_R_parentConstraint.cry" "knee_R.ry";
connectAttr "knee_R_parentConstraint.crz" "knee_R.rz";
connectAttr "knee_R.s" "ankle_R.is";
connectAttr "ankle_R_parentConstraint.ctx" "ankle_R.tx";
connectAttr "ankle_R_parentConstraint.cty" "ankle_R.ty";
connectAttr "ankle_R_parentConstraint.ctz" "ankle_R.tz";
connectAttr "ankle_R_parentConstraint.crx" "ankle_R.rx";
connectAttr "ankle_R_parentConstraint.cry" "ankle_R.ry";
connectAttr "ankle_R_parentConstraint.crz" "ankle_R.rz";
connectAttr "ankle_R.s" "footEnd_R.is";
connectAttr "ankle_R.ro" "ankle_R_parentConstraint.cro";
connectAttr "ankle_R.pim" "ankle_R_parentConstraint.cpim";
connectAttr "ankle_R.rp" "ankle_R_parentConstraint.crp";
connectAttr "ankle_R.rpt" "ankle_R_parentConstraint.crt";
connectAttr "ankle_R.jo" "ankle_R_parentConstraint.cjo";
connectAttr "ankle_R_CTL.t" "ankle_R_parentConstraint.tg[0].tt";
connectAttr "ankle_R_CTL.rp" "ankle_R_parentConstraint.tg[0].trp";
connectAttr "ankle_R_CTL.rpt" "ankle_R_parentConstraint.tg[0].trt";
connectAttr "ankle_R_CTL.r" "ankle_R_parentConstraint.tg[0].tr";
connectAttr "ankle_R_CTL.ro" "ankle_R_parentConstraint.tg[0].tro";
connectAttr "ankle_R_CTL.s" "ankle_R_parentConstraint.tg[0].ts";
connectAttr "ankle_R_CTL.pm" "ankle_R_parentConstraint.tg[0].tpm";
connectAttr "ankle_R_parentConstraint.w0" "ankle_R_parentConstraint.tg[0].tw";
connectAttr "knee_R.ro" "knee_R_parentConstraint.cro";
connectAttr "knee_R.pim" "knee_R_parentConstraint.cpim";
connectAttr "knee_R.rp" "knee_R_parentConstraint.crp";
connectAttr "knee_R.rpt" "knee_R_parentConstraint.crt";
connectAttr "knee_R.jo" "knee_R_parentConstraint.cjo";
connectAttr "knee_R_CTL.t" "knee_R_parentConstraint.tg[0].tt";
connectAttr "knee_R_CTL.rp" "knee_R_parentConstraint.tg[0].trp";
connectAttr "knee_R_CTL.rpt" "knee_R_parentConstraint.tg[0].trt";
connectAttr "knee_R_CTL.r" "knee_R_parentConstraint.tg[0].tr";
connectAttr "knee_R_CTL.ro" "knee_R_parentConstraint.tg[0].tro";
connectAttr "knee_R_CTL.s" "knee_R_parentConstraint.tg[0].ts";
connectAttr "knee_R_CTL.pm" "knee_R_parentConstraint.tg[0].tpm";
connectAttr "knee_R_parentConstraint.w0" "knee_R_parentConstraint.tg[0].tw";
connectAttr "leg_R.ro" "leg_R_parentConstraint.cro";
connectAttr "leg_R.pim" "leg_R_parentConstraint.cpim";
connectAttr "leg_R.rp" "leg_R_parentConstraint.crp";
connectAttr "leg_R.rpt" "leg_R_parentConstraint.crt";
connectAttr "leg_R.jo" "leg_R_parentConstraint.cjo";
connectAttr "leg_R_CTL.t" "leg_R_parentConstraint.tg[0].tt";
connectAttr "leg_R_CTL.rp" "leg_R_parentConstraint.tg[0].trp";
connectAttr "leg_R_CTL.rpt" "leg_R_parentConstraint.tg[0].trt";
connectAttr "leg_R_CTL.r" "leg_R_parentConstraint.tg[0].tr";
connectAttr "leg_R_CTL.ro" "leg_R_parentConstraint.tg[0].tro";
connectAttr "leg_R_CTL.s" "leg_R_parentConstraint.tg[0].ts";
connectAttr "leg_R_CTL.pm" "leg_R_parentConstraint.tg[0].tpm";
connectAttr "leg_R_parentConstraint.w0" "leg_R_parentConstraint.tg[0].tw";
connectAttr "root.ro" "root_parentConstraint.cro";
connectAttr "root.pim" "root_parentConstraint.cpim";
connectAttr "root.rp" "root_parentConstraint.crp";
connectAttr "root.rpt" "root_parentConstraint.crt";
connectAttr "root.jo" "root_parentConstraint.cjo";
connectAttr "cog_CTL.t" "root_parentConstraint.tg[0].tt";
connectAttr "cog_CTL.rp" "root_parentConstraint.tg[0].trp";
connectAttr "cog_CTL.rpt" "root_parentConstraint.tg[0].trt";
connectAttr "cog_CTL.r" "root_parentConstraint.tg[0].tr";
connectAttr "cog_CTL.ro" "root_parentConstraint.tg[0].tro";
connectAttr "cog_CTL.s" "root_parentConstraint.tg[0].ts";
connectAttr "cog_CTL.pm" "root_parentConstraint.tg[0].tpm";
connectAttr "root_parentConstraint.w0" "root_parentConstraint.tg[0].tw";
connectAttr "geometry_LYR.di" "geometry_GRP.do";
connectAttr "groupId17.id" "buddy_bodyShape.iog.og[0].gid";
connectAttr "topoSymmetrySet.mwc" "buddy_bodyShape.iog.og[0].gco";
connectAttr "buddy_body_skin.og[0]" "buddy_bodyShape.i";
connectAttr "buddy_teeth_skin.og[0]" "buddy_teethShape.i";
connectAttr "buddy_tongue_skin.og[0]" "buddy_tongueShape.i";
connectAttr "groupId18.id" "buddy_eye_lShape.iog.og[2].gid";
connectAttr "lambert5SG.mwc" "buddy_eye_lShape.iog.og[2].gco";
connectAttr "groupId19.id" "buddy_eye_lShape.iog.og[3].gid";
connectAttr "lambert2SG.mwc" "buddy_eye_lShape.iog.og[3].gco";
connectAttr "buddy_eye_l_skin.og[0]" "buddy_eye_lShape.i";
connectAttr "groupId20.id" "buddy_eye_rShape.iog.og[2].gid";
connectAttr "lambert5SG.mwc" "buddy_eye_rShape.iog.og[2].gco";
connectAttr "groupId21.id" "buddy_eye_rShape.iog.og[3].gid";
connectAttr "lambert2SG.mwc" "buddy_eye_rShape.iog.og[3].gco";
connectAttr "buddy_eye_r_skin.og[0]" "buddy_eye_rShape.i";
connectAttr "layerManager.dli[1]" "skeleton_LYR.id";
connectAttr "layerManager.dli[2]" "geometry_LYR.id";
connectAttr "groupId17.msg" "topoSymmetrySet.gn" -na;
connectAttr "buddy_bodyShape.iog.og[0]" "topoSymmetrySet.dsm" -na;
connectAttr "buddy_body_skinGroupParts.og" "buddy_body_skin.ip[0].ig";
connectAttr "buddy_bodyShapeOrig.o" "buddy_body_skin.orggeom[0]";
connectAttr "bindPose1.msg" "buddy_body_skin.bp";
connectAttr "ankle_L.wm" "buddy_body_skin.ma[0]";
connectAttr "ankle_R.wm" "buddy_body_skin.ma[1]";
connectAttr "head.wm" "buddy_body_skin.ma[2]";
connectAttr "hips.wm" "buddy_body_skin.ma[3]";
connectAttr "knee_L.wm" "buddy_body_skin.ma[4]";
connectAttr "knee_R.wm" "buddy_body_skin.ma[5]";
connectAttr "leg_L.wm" "buddy_body_skin.ma[6]";
connectAttr "leg_R.wm" "buddy_body_skin.ma[7]";
connectAttr "loElbow_L.wm" "buddy_body_skin.ma[8]";
connectAttr "loElbow_R.wm" "buddy_body_skin.ma[9]";
connectAttr "loFingerA1_L.wm" "buddy_body_skin.ma[10]";
connectAttr "loFingerA1_R.wm" "buddy_body_skin.ma[11]";
connectAttr "loFingerA2_L.wm" "buddy_body_skin.ma[12]";
connectAttr "loFingerA2_R.wm" "buddy_body_skin.ma[13]";
connectAttr "loFingerB1_L.wm" "buddy_body_skin.ma[14]";
connectAttr "loFingerB1_R.wm" "buddy_body_skin.ma[15]";
connectAttr "loFingerB2_L.wm" "buddy_body_skin.ma[16]";
connectAttr "loFingerB2_R.wm" "buddy_body_skin.ma[17]";
connectAttr "loShoulder_L.wm" "buddy_body_skin.ma[18]";
connectAttr "loShoulder_R.wm" "buddy_body_skin.ma[19]";
connectAttr "loThumb1_L.wm" "buddy_body_skin.ma[20]";
connectAttr "loThumb1_R.wm" "buddy_body_skin.ma[21]";
connectAttr "loThumb2_L.wm" "buddy_body_skin.ma[22]";
connectAttr "loThumb2_R.wm" "buddy_body_skin.ma[23]";
connectAttr "loWrist_L.wm" "buddy_body_skin.ma[24]";
connectAttr "loWrist_R.wm" "buddy_body_skin.ma[25]";
connectAttr "spine.wm" "buddy_body_skin.ma[26]";
connectAttr "upElbow_L.wm" "buddy_body_skin.ma[27]";
connectAttr "upElbow_R.wm" "buddy_body_skin.ma[28]";
connectAttr "upFingerA1_L.wm" "buddy_body_skin.ma[29]";
connectAttr "upFingerA1_R.wm" "buddy_body_skin.ma[30]";
connectAttr "upFingerA2_L.wm" "buddy_body_skin.ma[31]";
connectAttr "upFingerA2_R.wm" "buddy_body_skin.ma[32]";
connectAttr "upFingerB1_L.wm" "buddy_body_skin.ma[33]";
connectAttr "upFingerB1_R.wm" "buddy_body_skin.ma[34]";
connectAttr "upFingerB2_L.wm" "buddy_body_skin.ma[35]";
connectAttr "upFingerB2_R.wm" "buddy_body_skin.ma[36]";
connectAttr "upShoulder_L.wm" "buddy_body_skin.ma[37]";
connectAttr "upShoulder_R.wm" "buddy_body_skin.ma[38]";
connectAttr "upThumb1_L.wm" "buddy_body_skin.ma[39]";
connectAttr "upThumb1_R.wm" "buddy_body_skin.ma[40]";
connectAttr "upThumb2_L.wm" "buddy_body_skin.ma[41]";
connectAttr "upThumb2_R.wm" "buddy_body_skin.ma[42]";
connectAttr "upWrist_L.wm" "buddy_body_skin.ma[43]";
connectAttr "upWrist_R.wm" "buddy_body_skin.ma[44]";
connectAttr "ankle_L.liw" "buddy_body_skin.lw[0]";
connectAttr "ankle_R.liw" "buddy_body_skin.lw[1]";
connectAttr "head.liw" "buddy_body_skin.lw[2]";
connectAttr "hips.liw" "buddy_body_skin.lw[3]";
connectAttr "knee_L.liw" "buddy_body_skin.lw[4]";
connectAttr "knee_R.liw" "buddy_body_skin.lw[5]";
connectAttr "leg_L.liw" "buddy_body_skin.lw[6]";
connectAttr "leg_R.liw" "buddy_body_skin.lw[7]";
connectAttr "loElbow_L.liw" "buddy_body_skin.lw[8]";
connectAttr "loElbow_R.liw" "buddy_body_skin.lw[9]";
connectAttr "loFingerA1_L.liw" "buddy_body_skin.lw[10]";
connectAttr "loFingerA1_R.liw" "buddy_body_skin.lw[11]";
connectAttr "loFingerA2_L.liw" "buddy_body_skin.lw[12]";
connectAttr "loFingerA2_R.liw" "buddy_body_skin.lw[13]";
connectAttr "loFingerB1_L.liw" "buddy_body_skin.lw[14]";
connectAttr "loFingerB1_R.liw" "buddy_body_skin.lw[15]";
connectAttr "loFingerB2_L.liw" "buddy_body_skin.lw[16]";
connectAttr "loFingerB2_R.liw" "buddy_body_skin.lw[17]";
connectAttr "loShoulder_L.liw" "buddy_body_skin.lw[18]";
connectAttr "loShoulder_R.liw" "buddy_body_skin.lw[19]";
connectAttr "loThumb1_L.liw" "buddy_body_skin.lw[20]";
connectAttr "loThumb1_R.liw" "buddy_body_skin.lw[21]";
connectAttr "loThumb2_L.liw" "buddy_body_skin.lw[22]";
connectAttr "loThumb2_R.liw" "buddy_body_skin.lw[23]";
connectAttr "loWrist_L.liw" "buddy_body_skin.lw[24]";
connectAttr "loWrist_R.liw" "buddy_body_skin.lw[25]";
connectAttr "spine.liw" "buddy_body_skin.lw[26]";
connectAttr "upElbow_L.liw" "buddy_body_skin.lw[27]";
connectAttr "upElbow_R.liw" "buddy_body_skin.lw[28]";
connectAttr "upFingerA1_L.liw" "buddy_body_skin.lw[29]";
connectAttr "upFingerA1_R.liw" "buddy_body_skin.lw[30]";
connectAttr "upFingerA2_L.liw" "buddy_body_skin.lw[31]";
connectAttr "upFingerA2_R.liw" "buddy_body_skin.lw[32]";
connectAttr "upFingerB1_L.liw" "buddy_body_skin.lw[33]";
connectAttr "upFingerB1_R.liw" "buddy_body_skin.lw[34]";
connectAttr "upFingerB2_L.liw" "buddy_body_skin.lw[35]";
connectAttr "upFingerB2_R.liw" "buddy_body_skin.lw[36]";
connectAttr "upShoulder_L.liw" "buddy_body_skin.lw[37]";
connectAttr "upShoulder_R.liw" "buddy_body_skin.lw[38]";
connectAttr "upThumb1_L.liw" "buddy_body_skin.lw[39]";
connectAttr "upThumb1_R.liw" "buddy_body_skin.lw[40]";
connectAttr "upThumb2_L.liw" "buddy_body_skin.lw[41]";
connectAttr "upThumb2_R.liw" "buddy_body_skin.lw[42]";
connectAttr "upWrist_L.liw" "buddy_body_skin.lw[43]";
connectAttr "upWrist_R.liw" "buddy_body_skin.lw[44]";
connectAttr "ankle_L.obcc" "buddy_body_skin.ifcl[0]";
connectAttr "ankle_R.obcc" "buddy_body_skin.ifcl[1]";
connectAttr "head.obcc" "buddy_body_skin.ifcl[2]";
connectAttr "hips.obcc" "buddy_body_skin.ifcl[3]";
connectAttr "knee_L.obcc" "buddy_body_skin.ifcl[4]";
connectAttr "knee_R.obcc" "buddy_body_skin.ifcl[5]";
connectAttr "leg_L.obcc" "buddy_body_skin.ifcl[6]";
connectAttr "leg_R.obcc" "buddy_body_skin.ifcl[7]";
connectAttr "loElbow_L.obcc" "buddy_body_skin.ifcl[8]";
connectAttr "loElbow_R.obcc" "buddy_body_skin.ifcl[9]";
connectAttr "loFingerA1_L.obcc" "buddy_body_skin.ifcl[10]";
connectAttr "loFingerA1_R.obcc" "buddy_body_skin.ifcl[11]";
connectAttr "loFingerA2_L.obcc" "buddy_body_skin.ifcl[12]";
connectAttr "loFingerA2_R.obcc" "buddy_body_skin.ifcl[13]";
connectAttr "loFingerB1_L.obcc" "buddy_body_skin.ifcl[14]";
connectAttr "loFingerB1_R.obcc" "buddy_body_skin.ifcl[15]";
connectAttr "loFingerB2_L.obcc" "buddy_body_skin.ifcl[16]";
connectAttr "loFingerB2_R.obcc" "buddy_body_skin.ifcl[17]";
connectAttr "loShoulder_L.obcc" "buddy_body_skin.ifcl[18]";
connectAttr "loShoulder_R.obcc" "buddy_body_skin.ifcl[19]";
connectAttr "loThumb1_L.obcc" "buddy_body_skin.ifcl[20]";
connectAttr "loThumb1_R.obcc" "buddy_body_skin.ifcl[21]";
connectAttr "loThumb2_L.obcc" "buddy_body_skin.ifcl[22]";
connectAttr "loThumb2_R.obcc" "buddy_body_skin.ifcl[23]";
connectAttr "loWrist_L.obcc" "buddy_body_skin.ifcl[24]";
connectAttr "loWrist_R.obcc" "buddy_body_skin.ifcl[25]";
connectAttr "spine.obcc" "buddy_body_skin.ifcl[26]";
connectAttr "upElbow_L.obcc" "buddy_body_skin.ifcl[27]";
connectAttr "upElbow_R.obcc" "buddy_body_skin.ifcl[28]";
connectAttr "upFingerA1_L.obcc" "buddy_body_skin.ifcl[29]";
connectAttr "upFingerA1_R.obcc" "buddy_body_skin.ifcl[30]";
connectAttr "upFingerA2_L.obcc" "buddy_body_skin.ifcl[31]";
connectAttr "upFingerA2_R.obcc" "buddy_body_skin.ifcl[32]";
connectAttr "upFingerB1_L.obcc" "buddy_body_skin.ifcl[33]";
connectAttr "upFingerB1_R.obcc" "buddy_body_skin.ifcl[34]";
connectAttr "upFingerB2_L.obcc" "buddy_body_skin.ifcl[35]";
connectAttr "upFingerB2_R.obcc" "buddy_body_skin.ifcl[36]";
connectAttr "upShoulder_L.obcc" "buddy_body_skin.ifcl[37]";
connectAttr "upShoulder_R.obcc" "buddy_body_skin.ifcl[38]";
connectAttr "upThumb1_L.obcc" "buddy_body_skin.ifcl[39]";
connectAttr "upThumb1_R.obcc" "buddy_body_skin.ifcl[40]";
connectAttr "upThumb2_L.obcc" "buddy_body_skin.ifcl[41]";
connectAttr "upThumb2_R.obcc" "buddy_body_skin.ifcl[42]";
connectAttr "upWrist_L.obcc" "buddy_body_skin.ifcl[43]";
connectAttr "upWrist_R.obcc" "buddy_body_skin.ifcl[44]";
connectAttr "root.msg" "bindPose1.m[0]";
connectAttr "leg_L.msg" "bindPose1.m[1]";
connectAttr "knee_L.msg" "bindPose1.m[2]";
connectAttr "ankle_L.msg" "bindPose1.m[3]";
connectAttr "leg_R.msg" "bindPose1.m[4]";
connectAttr "knee_R.msg" "bindPose1.m[5]";
connectAttr "ankle_R.msg" "bindPose1.m[6]";
connectAttr "hips.msg" "bindPose1.m[7]";
connectAttr "spine.msg" "bindPose1.m[8]";
connectAttr "head.msg" "bindPose1.m[9]";
connectAttr "loShoulder_L.msg" "bindPose1.m[10]";
connectAttr "loElbow_L.msg" "bindPose1.m[11]";
connectAttr "loShoulder_R.msg" "bindPose1.m[12]";
connectAttr "loElbow_R.msg" "bindPose1.m[13]";
connectAttr "loWrist_L.msg" "bindPose1.m[14]";
connectAttr "loFingerA1_L.msg" "bindPose1.m[15]";
connectAttr "loWrist_R.msg" "bindPose1.m[16]";
connectAttr "loFingerA1_R.msg" "bindPose1.m[17]";
connectAttr "loFingerA2_L.msg" "bindPose1.m[18]";
connectAttr "loFingerA2_R.msg" "bindPose1.m[19]";
connectAttr "loFingerB1_L.msg" "bindPose1.m[20]";
connectAttr "loFingerB1_R.msg" "bindPose1.m[21]";
connectAttr "loFingerB2_L.msg" "bindPose1.m[22]";
connectAttr "loFingerB2_R.msg" "bindPose1.m[23]";
connectAttr "loThumb1_L.msg" "bindPose1.m[24]";
connectAttr "loThumb1_R.msg" "bindPose1.m[25]";
connectAttr "loThumb2_L.msg" "bindPose1.m[26]";
connectAttr "loThumb2_R.msg" "bindPose1.m[27]";
connectAttr "upShoulder_L.msg" "bindPose1.m[28]";
connectAttr "upElbow_L.msg" "bindPose1.m[29]";
connectAttr "upShoulder_R.msg" "bindPose1.m[30]";
connectAttr "upElbow_R.msg" "bindPose1.m[31]";
connectAttr "upWrist_L.msg" "bindPose1.m[32]";
connectAttr "upFingerA1_L.msg" "bindPose1.m[33]";
connectAttr "upWrist_R.msg" "bindPose1.m[34]";
connectAttr "upFingerA1_R.msg" "bindPose1.m[35]";
connectAttr "upFingerA2_L.msg" "bindPose1.m[36]";
connectAttr "upFingerA2_R.msg" "bindPose1.m[37]";
connectAttr "upFingerB1_L.msg" "bindPose1.m[38]";
connectAttr "upFingerB1_R.msg" "bindPose1.m[39]";
connectAttr "upFingerB2_L.msg" "bindPose1.m[40]";
connectAttr "upFingerB2_R.msg" "bindPose1.m[41]";
connectAttr "upThumb1_L.msg" "bindPose1.m[42]";
connectAttr "upThumb1_R.msg" "bindPose1.m[43]";
connectAttr "upThumb2_L.msg" "bindPose1.m[44]";
connectAttr "upThumb2_R.msg" "bindPose1.m[45]";
connectAttr "bindPose1.w" "bindPose1.p[0]";
connectAttr "bindPose1.m[0]" "bindPose1.p[1]";
connectAttr "bindPose1.m[1]" "bindPose1.p[2]";
connectAttr "bindPose1.m[2]" "bindPose1.p[3]";
connectAttr "bindPose1.m[0]" "bindPose1.p[4]";
connectAttr "bindPose1.m[4]" "bindPose1.p[5]";
connectAttr "bindPose1.m[5]" "bindPose1.p[6]";
connectAttr "bindPose1.m[0]" "bindPose1.p[7]";
connectAttr "bindPose1.m[7]" "bindPose1.p[8]";
connectAttr "bindPose1.m[8]" "bindPose1.p[9]";
connectAttr "bindPose1.m[8]" "bindPose1.p[10]";
connectAttr "bindPose1.m[10]" "bindPose1.p[11]";
connectAttr "bindPose1.m[8]" "bindPose1.p[12]";
connectAttr "bindPose1.m[12]" "bindPose1.p[13]";
connectAttr "bindPose1.m[11]" "bindPose1.p[14]";
connectAttr "bindPose1.m[14]" "bindPose1.p[15]";
connectAttr "bindPose1.m[13]" "bindPose1.p[16]";
connectAttr "bindPose1.m[16]" "bindPose1.p[17]";
connectAttr "bindPose1.m[15]" "bindPose1.p[18]";
connectAttr "bindPose1.m[17]" "bindPose1.p[19]";
connectAttr "bindPose1.m[14]" "bindPose1.p[20]";
connectAttr "bindPose1.m[16]" "bindPose1.p[21]";
connectAttr "bindPose1.m[20]" "bindPose1.p[22]";
connectAttr "bindPose1.m[21]" "bindPose1.p[23]";
connectAttr "bindPose1.m[14]" "bindPose1.p[24]";
connectAttr "bindPose1.m[16]" "bindPose1.p[25]";
connectAttr "bindPose1.m[24]" "bindPose1.p[26]";
connectAttr "bindPose1.m[25]" "bindPose1.p[27]";
connectAttr "bindPose1.m[9]" "bindPose1.p[28]";
connectAttr "bindPose1.m[28]" "bindPose1.p[29]";
connectAttr "bindPose1.m[9]" "bindPose1.p[30]";
connectAttr "bindPose1.m[30]" "bindPose1.p[31]";
connectAttr "bindPose1.m[29]" "bindPose1.p[32]";
connectAttr "bindPose1.m[32]" "bindPose1.p[33]";
connectAttr "bindPose1.m[31]" "bindPose1.p[34]";
connectAttr "bindPose1.m[34]" "bindPose1.p[35]";
connectAttr "bindPose1.m[33]" "bindPose1.p[36]";
connectAttr "bindPose1.m[35]" "bindPose1.p[37]";
connectAttr "bindPose1.m[32]" "bindPose1.p[38]";
connectAttr "bindPose1.m[34]" "bindPose1.p[39]";
connectAttr "bindPose1.m[38]" "bindPose1.p[40]";
connectAttr "bindPose1.m[39]" "bindPose1.p[41]";
connectAttr "bindPose1.m[32]" "bindPose1.p[42]";
connectAttr "bindPose1.m[34]" "bindPose1.p[43]";
connectAttr "bindPose1.m[42]" "bindPose1.p[44]";
connectAttr "bindPose1.m[43]" "bindPose1.p[45]";
connectAttr "root.bps" "bindPose1.wm[0]";
connectAttr "leg_L.bps" "bindPose1.wm[1]";
connectAttr "knee_L.bps" "bindPose1.wm[2]";
connectAttr "ankle_L.bps" "bindPose1.wm[3]";
connectAttr "leg_R.bps" "bindPose1.wm[4]";
connectAttr "knee_R.bps" "bindPose1.wm[5]";
connectAttr "ankle_R.bps" "bindPose1.wm[6]";
connectAttr "hips.bps" "bindPose1.wm[7]";
connectAttr "spine.bps" "bindPose1.wm[8]";
connectAttr "head.bps" "bindPose1.wm[9]";
connectAttr "loShoulder_L.bps" "bindPose1.wm[10]";
connectAttr "loElbow_L.bps" "bindPose1.wm[11]";
connectAttr "loShoulder_R.bps" "bindPose1.wm[12]";
connectAttr "loElbow_R.bps" "bindPose1.wm[13]";
connectAttr "loWrist_L.bps" "bindPose1.wm[14]";
connectAttr "loFingerA1_L.bps" "bindPose1.wm[15]";
connectAttr "loWrist_R.bps" "bindPose1.wm[16]";
connectAttr "loFingerA1_R.bps" "bindPose1.wm[17]";
connectAttr "loFingerA2_L.bps" "bindPose1.wm[18]";
connectAttr "loFingerA2_R.bps" "bindPose1.wm[19]";
connectAttr "loFingerB1_L.bps" "bindPose1.wm[20]";
connectAttr "loFingerB1_R.bps" "bindPose1.wm[21]";
connectAttr "loFingerB2_L.bps" "bindPose1.wm[22]";
connectAttr "loFingerB2_R.bps" "bindPose1.wm[23]";
connectAttr "loThumb1_L.bps" "bindPose1.wm[24]";
connectAttr "loThumb1_R.bps" "bindPose1.wm[25]";
connectAttr "loThumb2_L.bps" "bindPose1.wm[26]";
connectAttr "loThumb2_R.bps" "bindPose1.wm[27]";
connectAttr "upShoulder_L.bps" "bindPose1.wm[28]";
connectAttr "upElbow_L.bps" "bindPose1.wm[29]";
connectAttr "upShoulder_R.bps" "bindPose1.wm[30]";
connectAttr "upElbow_R.bps" "bindPose1.wm[31]";
connectAttr "upWrist_L.bps" "bindPose1.wm[32]";
connectAttr "upFingerA1_L.bps" "bindPose1.wm[33]";
connectAttr "upWrist_R.bps" "bindPose1.wm[34]";
connectAttr "upFingerA1_R.bps" "bindPose1.wm[35]";
connectAttr "upFingerA2_L.bps" "bindPose1.wm[36]";
connectAttr "upFingerA2_R.bps" "bindPose1.wm[37]";
connectAttr "upFingerB1_L.bps" "bindPose1.wm[38]";
connectAttr "upFingerB1_R.bps" "bindPose1.wm[39]";
connectAttr "upFingerB2_L.bps" "bindPose1.wm[40]";
connectAttr "upFingerB2_R.bps" "bindPose1.wm[41]";
connectAttr "upThumb1_L.bps" "bindPose1.wm[42]";
connectAttr "upThumb1_R.bps" "bindPose1.wm[43]";
connectAttr "upThumb2_L.bps" "bindPose1.wm[44]";
connectAttr "upThumb2_R.bps" "bindPose1.wm[45]";
connectAttr "root.msg" "bindPose2.m[0]";
connectAttr "hips.msg" "bindPose2.m[1]";
connectAttr "spine.msg" "bindPose2.m[2]";
connectAttr "head.msg" "bindPose2.m[3]";
connectAttr "eye_L.msg" "bindPose2.m[4]";
connectAttr "bindPose2.w" "bindPose2.p[0]";
connectAttr "bindPose2.m[0]" "bindPose2.p[1]";
connectAttr "bindPose2.m[1]" "bindPose2.p[2]";
connectAttr "bindPose2.m[2]" "bindPose2.p[3]";
connectAttr "bindPose2.m[3]" "bindPose2.p[4]";
connectAttr "root.msg" "bindPose3.m[0]";
connectAttr "hips.msg" "bindPose3.m[1]";
connectAttr "spine.msg" "bindPose3.m[2]";
connectAttr "head.msg" "bindPose3.m[3]";
connectAttr "eye_R.msg" "bindPose3.m[4]";
connectAttr "bindPose3.w" "bindPose3.p[0]";
connectAttr "bindPose3.m[0]" "bindPose3.p[1]";
connectAttr "bindPose3.m[1]" "bindPose3.p[2]";
connectAttr "bindPose3.m[2]" "bindPose3.p[3]";
connectAttr "bindPose3.m[3]" "bindPose3.p[4]";
connectAttr "buddy_bodyShapeOrig.w" "buddy_body_skinGroupParts.ig";
connectAttr "groupId17.id" "buddy_body_skinGroupParts.gi";
connectAttr "lambert3SG.msg" "materialInfo2.sg";
connectAttr "lambert3.msg" "materialInfo2.m";
connectAttr "lambert3.oc" "lambert3SG.ss";
connectAttr "buddy_bodyShape.iog" "lambert3SG.dsm" -na;
connectAttr "buddy_teethShapeOrig.w" "buddy_teeth_skin.ip[0].ig";
connectAttr "buddy_teethShapeOrig.o" "buddy_teeth_skin.orggeom[0]";
connectAttr "ankle_L.wm" "buddy_teeth_skin.ma[0]";
connectAttr "ankle_R.wm" "buddy_teeth_skin.ma[1]";
connectAttr "head.wm" "buddy_teeth_skin.ma[2]";
connectAttr "hips.wm" "buddy_teeth_skin.ma[3]";
connectAttr "knee_L.wm" "buddy_teeth_skin.ma[4]";
connectAttr "knee_R.wm" "buddy_teeth_skin.ma[5]";
connectAttr "leg_L.wm" "buddy_teeth_skin.ma[6]";
connectAttr "leg_R.wm" "buddy_teeth_skin.ma[7]";
connectAttr "loElbow_L.wm" "buddy_teeth_skin.ma[8]";
connectAttr "loElbow_R.wm" "buddy_teeth_skin.ma[9]";
connectAttr "loFingerA1_L.wm" "buddy_teeth_skin.ma[10]";
connectAttr "loFingerA1_R.wm" "buddy_teeth_skin.ma[11]";
connectAttr "loFingerA2_L.wm" "buddy_teeth_skin.ma[12]";
connectAttr "loFingerA2_R.wm" "buddy_teeth_skin.ma[13]";
connectAttr "loFingerB1_L.wm" "buddy_teeth_skin.ma[14]";
connectAttr "loFingerB1_R.wm" "buddy_teeth_skin.ma[15]";
connectAttr "loFingerB2_L.wm" "buddy_teeth_skin.ma[16]";
connectAttr "loFingerB2_R.wm" "buddy_teeth_skin.ma[17]";
connectAttr "loShoulder_L.wm" "buddy_teeth_skin.ma[18]";
connectAttr "loShoulder_R.wm" "buddy_teeth_skin.ma[19]";
connectAttr "loThumb1_L.wm" "buddy_teeth_skin.ma[20]";
connectAttr "loThumb1_R.wm" "buddy_teeth_skin.ma[21]";
connectAttr "loThumb2_L.wm" "buddy_teeth_skin.ma[22]";
connectAttr "loThumb2_R.wm" "buddy_teeth_skin.ma[23]";
connectAttr "loWrist_L.wm" "buddy_teeth_skin.ma[24]";
connectAttr "loWrist_R.wm" "buddy_teeth_skin.ma[25]";
connectAttr "spine.wm" "buddy_teeth_skin.ma[26]";
connectAttr "upElbow_L.wm" "buddy_teeth_skin.ma[27]";
connectAttr "upElbow_R.wm" "buddy_teeth_skin.ma[28]";
connectAttr "upFingerA1_L.wm" "buddy_teeth_skin.ma[29]";
connectAttr "upFingerA1_R.wm" "buddy_teeth_skin.ma[30]";
connectAttr "upFingerA2_L.wm" "buddy_teeth_skin.ma[31]";
connectAttr "upFingerA2_R.wm" "buddy_teeth_skin.ma[32]";
connectAttr "upFingerB1_L.wm" "buddy_teeth_skin.ma[33]";
connectAttr "upFingerB1_R.wm" "buddy_teeth_skin.ma[34]";
connectAttr "upFingerB2_L.wm" "buddy_teeth_skin.ma[35]";
connectAttr "upFingerB2_R.wm" "buddy_teeth_skin.ma[36]";
connectAttr "upShoulder_L.wm" "buddy_teeth_skin.ma[37]";
connectAttr "upShoulder_R.wm" "buddy_teeth_skin.ma[38]";
connectAttr "upThumb1_L.wm" "buddy_teeth_skin.ma[39]";
connectAttr "upThumb1_R.wm" "buddy_teeth_skin.ma[40]";
connectAttr "upThumb2_L.wm" "buddy_teeth_skin.ma[41]";
connectAttr "upThumb2_R.wm" "buddy_teeth_skin.ma[42]";
connectAttr "upWrist_L.wm" "buddy_teeth_skin.ma[43]";
connectAttr "upWrist_R.wm" "buddy_teeth_skin.ma[44]";
connectAttr "ankle_L.liw" "buddy_teeth_skin.lw[0]";
connectAttr "ankle_R.liw" "buddy_teeth_skin.lw[1]";
connectAttr "head.liw" "buddy_teeth_skin.lw[2]";
connectAttr "hips.liw" "buddy_teeth_skin.lw[3]";
connectAttr "knee_L.liw" "buddy_teeth_skin.lw[4]";
connectAttr "knee_R.liw" "buddy_teeth_skin.lw[5]";
connectAttr "leg_L.liw" "buddy_teeth_skin.lw[6]";
connectAttr "leg_R.liw" "buddy_teeth_skin.lw[7]";
connectAttr "loElbow_L.liw" "buddy_teeth_skin.lw[8]";
connectAttr "loElbow_R.liw" "buddy_teeth_skin.lw[9]";
connectAttr "loFingerA1_L.liw" "buddy_teeth_skin.lw[10]";
connectAttr "loFingerA1_R.liw" "buddy_teeth_skin.lw[11]";
connectAttr "loFingerA2_L.liw" "buddy_teeth_skin.lw[12]";
connectAttr "loFingerA2_R.liw" "buddy_teeth_skin.lw[13]";
connectAttr "loFingerB1_L.liw" "buddy_teeth_skin.lw[14]";
connectAttr "loFingerB1_R.liw" "buddy_teeth_skin.lw[15]";
connectAttr "loFingerB2_L.liw" "buddy_teeth_skin.lw[16]";
connectAttr "loFingerB2_R.liw" "buddy_teeth_skin.lw[17]";
connectAttr "loShoulder_L.liw" "buddy_teeth_skin.lw[18]";
connectAttr "loShoulder_R.liw" "buddy_teeth_skin.lw[19]";
connectAttr "loThumb1_L.liw" "buddy_teeth_skin.lw[20]";
connectAttr "loThumb1_R.liw" "buddy_teeth_skin.lw[21]";
connectAttr "loThumb2_L.liw" "buddy_teeth_skin.lw[22]";
connectAttr "loThumb2_R.liw" "buddy_teeth_skin.lw[23]";
connectAttr "loWrist_L.liw" "buddy_teeth_skin.lw[24]";
connectAttr "loWrist_R.liw" "buddy_teeth_skin.lw[25]";
connectAttr "spine.liw" "buddy_teeth_skin.lw[26]";
connectAttr "upElbow_L.liw" "buddy_teeth_skin.lw[27]";
connectAttr "upElbow_R.liw" "buddy_teeth_skin.lw[28]";
connectAttr "upFingerA1_L.liw" "buddy_teeth_skin.lw[29]";
connectAttr "upFingerA1_R.liw" "buddy_teeth_skin.lw[30]";
connectAttr "upFingerA2_L.liw" "buddy_teeth_skin.lw[31]";
connectAttr "upFingerA2_R.liw" "buddy_teeth_skin.lw[32]";
connectAttr "upFingerB1_L.liw" "buddy_teeth_skin.lw[33]";
connectAttr "upFingerB1_R.liw" "buddy_teeth_skin.lw[34]";
connectAttr "upFingerB2_L.liw" "buddy_teeth_skin.lw[35]";
connectAttr "upFingerB2_R.liw" "buddy_teeth_skin.lw[36]";
connectAttr "upShoulder_L.liw" "buddy_teeth_skin.lw[37]";
connectAttr "upShoulder_R.liw" "buddy_teeth_skin.lw[38]";
connectAttr "upThumb1_L.liw" "buddy_teeth_skin.lw[39]";
connectAttr "upThumb1_R.liw" "buddy_teeth_skin.lw[40]";
connectAttr "upThumb2_L.liw" "buddy_teeth_skin.lw[41]";
connectAttr "upThumb2_R.liw" "buddy_teeth_skin.lw[42]";
connectAttr "upWrist_L.liw" "buddy_teeth_skin.lw[43]";
connectAttr "upWrist_R.liw" "buddy_teeth_skin.lw[44]";
connectAttr "ankle_L.obcc" "buddy_teeth_skin.ifcl[0]";
connectAttr "ankle_R.obcc" "buddy_teeth_skin.ifcl[1]";
connectAttr "head.obcc" "buddy_teeth_skin.ifcl[2]";
connectAttr "hips.obcc" "buddy_teeth_skin.ifcl[3]";
connectAttr "knee_L.obcc" "buddy_teeth_skin.ifcl[4]";
connectAttr "knee_R.obcc" "buddy_teeth_skin.ifcl[5]";
connectAttr "leg_L.obcc" "buddy_teeth_skin.ifcl[6]";
connectAttr "leg_R.obcc" "buddy_teeth_skin.ifcl[7]";
connectAttr "loElbow_L.obcc" "buddy_teeth_skin.ifcl[8]";
connectAttr "loElbow_R.obcc" "buddy_teeth_skin.ifcl[9]";
connectAttr "loFingerA1_L.obcc" "buddy_teeth_skin.ifcl[10]";
connectAttr "loFingerA1_R.obcc" "buddy_teeth_skin.ifcl[11]";
connectAttr "loFingerA2_L.obcc" "buddy_teeth_skin.ifcl[12]";
connectAttr "loFingerA2_R.obcc" "buddy_teeth_skin.ifcl[13]";
connectAttr "loFingerB1_L.obcc" "buddy_teeth_skin.ifcl[14]";
connectAttr "loFingerB1_R.obcc" "buddy_teeth_skin.ifcl[15]";
connectAttr "loFingerB2_L.obcc" "buddy_teeth_skin.ifcl[16]";
connectAttr "loFingerB2_R.obcc" "buddy_teeth_skin.ifcl[17]";
connectAttr "loShoulder_L.obcc" "buddy_teeth_skin.ifcl[18]";
connectAttr "loShoulder_R.obcc" "buddy_teeth_skin.ifcl[19]";
connectAttr "loThumb1_L.obcc" "buddy_teeth_skin.ifcl[20]";
connectAttr "loThumb1_R.obcc" "buddy_teeth_skin.ifcl[21]";
connectAttr "loThumb2_L.obcc" "buddy_teeth_skin.ifcl[22]";
connectAttr "loThumb2_R.obcc" "buddy_teeth_skin.ifcl[23]";
connectAttr "loWrist_L.obcc" "buddy_teeth_skin.ifcl[24]";
connectAttr "loWrist_R.obcc" "buddy_teeth_skin.ifcl[25]";
connectAttr "spine.obcc" "buddy_teeth_skin.ifcl[26]";
connectAttr "upElbow_L.obcc" "buddy_teeth_skin.ifcl[27]";
connectAttr "upElbow_R.obcc" "buddy_teeth_skin.ifcl[28]";
connectAttr "upFingerA1_L.obcc" "buddy_teeth_skin.ifcl[29]";
connectAttr "upFingerA1_R.obcc" "buddy_teeth_skin.ifcl[30]";
connectAttr "upFingerA2_L.obcc" "buddy_teeth_skin.ifcl[31]";
connectAttr "upFingerA2_R.obcc" "buddy_teeth_skin.ifcl[32]";
connectAttr "upFingerB1_L.obcc" "buddy_teeth_skin.ifcl[33]";
connectAttr "upFingerB1_R.obcc" "buddy_teeth_skin.ifcl[34]";
connectAttr "upFingerB2_L.obcc" "buddy_teeth_skin.ifcl[35]";
connectAttr "upFingerB2_R.obcc" "buddy_teeth_skin.ifcl[36]";
connectAttr "upShoulder_L.obcc" "buddy_teeth_skin.ifcl[37]";
connectAttr "upShoulder_R.obcc" "buddy_teeth_skin.ifcl[38]";
connectAttr "upThumb1_L.obcc" "buddy_teeth_skin.ifcl[39]";
connectAttr "upThumb1_R.obcc" "buddy_teeth_skin.ifcl[40]";
connectAttr "upThumb2_L.obcc" "buddy_teeth_skin.ifcl[41]";
connectAttr "upThumb2_R.obcc" "buddy_teeth_skin.ifcl[42]";
connectAttr "upWrist_L.obcc" "buddy_teeth_skin.ifcl[43]";
connectAttr "upWrist_R.obcc" "buddy_teeth_skin.ifcl[44]";
connectAttr "bindPose1.msg" "buddy_teeth_skin.bp";
connectAttr "lambert5SG.msg" "materialInfo4.sg";
connectAttr "lambert5.msg" "materialInfo4.m";
connectAttr "lambert5.oc" "lambert5SG.ss";
connectAttr "buddy_teethShape.iog" "lambert5SG.dsm" -na;
connectAttr "buddy_eye_lShape.iog.og[2]" "lambert5SG.dsm" -na;
connectAttr "buddy_eye_rShape.iog.og[2]" "lambert5SG.dsm" -na;
connectAttr "groupId18.msg" "lambert5SG.gn" -na;
connectAttr "groupId20.msg" "lambert5SG.gn" -na;
connectAttr "buddy_tongueShapeOrig.w" "buddy_tongue_skin.ip[0].ig";
connectAttr "buddy_tongueShapeOrig.o" "buddy_tongue_skin.orggeom[0]";
connectAttr "ankle_L.wm" "buddy_tongue_skin.ma[0]";
connectAttr "ankle_R.wm" "buddy_tongue_skin.ma[1]";
connectAttr "head.wm" "buddy_tongue_skin.ma[2]";
connectAttr "hips.wm" "buddy_tongue_skin.ma[3]";
connectAttr "knee_L.wm" "buddy_tongue_skin.ma[4]";
connectAttr "knee_R.wm" "buddy_tongue_skin.ma[5]";
connectAttr "leg_L.wm" "buddy_tongue_skin.ma[6]";
connectAttr "leg_R.wm" "buddy_tongue_skin.ma[7]";
connectAttr "loElbow_L.wm" "buddy_tongue_skin.ma[8]";
connectAttr "loElbow_R.wm" "buddy_tongue_skin.ma[9]";
connectAttr "loFingerA1_L.wm" "buddy_tongue_skin.ma[10]";
connectAttr "loFingerA1_R.wm" "buddy_tongue_skin.ma[11]";
connectAttr "loFingerA2_L.wm" "buddy_tongue_skin.ma[12]";
connectAttr "loFingerA2_R.wm" "buddy_tongue_skin.ma[13]";
connectAttr "loFingerB1_L.wm" "buddy_tongue_skin.ma[14]";
connectAttr "loFingerB1_R.wm" "buddy_tongue_skin.ma[15]";
connectAttr "loFingerB2_L.wm" "buddy_tongue_skin.ma[16]";
connectAttr "loFingerB2_R.wm" "buddy_tongue_skin.ma[17]";
connectAttr "loShoulder_L.wm" "buddy_tongue_skin.ma[18]";
connectAttr "loShoulder_R.wm" "buddy_tongue_skin.ma[19]";
connectAttr "loThumb1_L.wm" "buddy_tongue_skin.ma[20]";
connectAttr "loThumb1_R.wm" "buddy_tongue_skin.ma[21]";
connectAttr "loThumb2_L.wm" "buddy_tongue_skin.ma[22]";
connectAttr "loThumb2_R.wm" "buddy_tongue_skin.ma[23]";
connectAttr "loWrist_L.wm" "buddy_tongue_skin.ma[24]";
connectAttr "loWrist_R.wm" "buddy_tongue_skin.ma[25]";
connectAttr "spine.wm" "buddy_tongue_skin.ma[26]";
connectAttr "upElbow_L.wm" "buddy_tongue_skin.ma[27]";
connectAttr "upElbow_R.wm" "buddy_tongue_skin.ma[28]";
connectAttr "upFingerA1_L.wm" "buddy_tongue_skin.ma[29]";
connectAttr "upFingerA1_R.wm" "buddy_tongue_skin.ma[30]";
connectAttr "upFingerA2_L.wm" "buddy_tongue_skin.ma[31]";
connectAttr "upFingerA2_R.wm" "buddy_tongue_skin.ma[32]";
connectAttr "upFingerB1_L.wm" "buddy_tongue_skin.ma[33]";
connectAttr "upFingerB1_R.wm" "buddy_tongue_skin.ma[34]";
connectAttr "upFingerB2_L.wm" "buddy_tongue_skin.ma[35]";
connectAttr "upFingerB2_R.wm" "buddy_tongue_skin.ma[36]";
connectAttr "upShoulder_L.wm" "buddy_tongue_skin.ma[37]";
connectAttr "upShoulder_R.wm" "buddy_tongue_skin.ma[38]";
connectAttr "upThumb1_L.wm" "buddy_tongue_skin.ma[39]";
connectAttr "upThumb1_R.wm" "buddy_tongue_skin.ma[40]";
connectAttr "upThumb2_L.wm" "buddy_tongue_skin.ma[41]";
connectAttr "upThumb2_R.wm" "buddy_tongue_skin.ma[42]";
connectAttr "upWrist_L.wm" "buddy_tongue_skin.ma[43]";
connectAttr "upWrist_R.wm" "buddy_tongue_skin.ma[44]";
connectAttr "ankle_L.liw" "buddy_tongue_skin.lw[0]";
connectAttr "ankle_R.liw" "buddy_tongue_skin.lw[1]";
connectAttr "head.liw" "buddy_tongue_skin.lw[2]";
connectAttr "hips.liw" "buddy_tongue_skin.lw[3]";
connectAttr "knee_L.liw" "buddy_tongue_skin.lw[4]";
connectAttr "knee_R.liw" "buddy_tongue_skin.lw[5]";
connectAttr "leg_L.liw" "buddy_tongue_skin.lw[6]";
connectAttr "leg_R.liw" "buddy_tongue_skin.lw[7]";
connectAttr "loElbow_L.liw" "buddy_tongue_skin.lw[8]";
connectAttr "loElbow_R.liw" "buddy_tongue_skin.lw[9]";
connectAttr "loFingerA1_L.liw" "buddy_tongue_skin.lw[10]";
connectAttr "loFingerA1_R.liw" "buddy_tongue_skin.lw[11]";
connectAttr "loFingerA2_L.liw" "buddy_tongue_skin.lw[12]";
connectAttr "loFingerA2_R.liw" "buddy_tongue_skin.lw[13]";
connectAttr "loFingerB1_L.liw" "buddy_tongue_skin.lw[14]";
connectAttr "loFingerB1_R.liw" "buddy_tongue_skin.lw[15]";
connectAttr "loFingerB2_L.liw" "buddy_tongue_skin.lw[16]";
connectAttr "loFingerB2_R.liw" "buddy_tongue_skin.lw[17]";
connectAttr "loShoulder_L.liw" "buddy_tongue_skin.lw[18]";
connectAttr "loShoulder_R.liw" "buddy_tongue_skin.lw[19]";
connectAttr "loThumb1_L.liw" "buddy_tongue_skin.lw[20]";
connectAttr "loThumb1_R.liw" "buddy_tongue_skin.lw[21]";
connectAttr "loThumb2_L.liw" "buddy_tongue_skin.lw[22]";
connectAttr "loThumb2_R.liw" "buddy_tongue_skin.lw[23]";
connectAttr "loWrist_L.liw" "buddy_tongue_skin.lw[24]";
connectAttr "loWrist_R.liw" "buddy_tongue_skin.lw[25]";
connectAttr "spine.liw" "buddy_tongue_skin.lw[26]";
connectAttr "upElbow_L.liw" "buddy_tongue_skin.lw[27]";
connectAttr "upElbow_R.liw" "buddy_tongue_skin.lw[28]";
connectAttr "upFingerA1_L.liw" "buddy_tongue_skin.lw[29]";
connectAttr "upFingerA1_R.liw" "buddy_tongue_skin.lw[30]";
connectAttr "upFingerA2_L.liw" "buddy_tongue_skin.lw[31]";
connectAttr "upFingerA2_R.liw" "buddy_tongue_skin.lw[32]";
connectAttr "upFingerB1_L.liw" "buddy_tongue_skin.lw[33]";
connectAttr "upFingerB1_R.liw" "buddy_tongue_skin.lw[34]";
connectAttr "upFingerB2_L.liw" "buddy_tongue_skin.lw[35]";
connectAttr "upFingerB2_R.liw" "buddy_tongue_skin.lw[36]";
connectAttr "upShoulder_L.liw" "buddy_tongue_skin.lw[37]";
connectAttr "upShoulder_R.liw" "buddy_tongue_skin.lw[38]";
connectAttr "upThumb1_L.liw" "buddy_tongue_skin.lw[39]";
connectAttr "upThumb1_R.liw" "buddy_tongue_skin.lw[40]";
connectAttr "upThumb2_L.liw" "buddy_tongue_skin.lw[41]";
connectAttr "upThumb2_R.liw" "buddy_tongue_skin.lw[42]";
connectAttr "upWrist_L.liw" "buddy_tongue_skin.lw[43]";
connectAttr "upWrist_R.liw" "buddy_tongue_skin.lw[44]";
connectAttr "ankle_L.obcc" "buddy_tongue_skin.ifcl[0]";
connectAttr "ankle_R.obcc" "buddy_tongue_skin.ifcl[1]";
connectAttr "head.obcc" "buddy_tongue_skin.ifcl[2]";
connectAttr "hips.obcc" "buddy_tongue_skin.ifcl[3]";
connectAttr "knee_L.obcc" "buddy_tongue_skin.ifcl[4]";
connectAttr "knee_R.obcc" "buddy_tongue_skin.ifcl[5]";
connectAttr "leg_L.obcc" "buddy_tongue_skin.ifcl[6]";
connectAttr "leg_R.obcc" "buddy_tongue_skin.ifcl[7]";
connectAttr "loElbow_L.obcc" "buddy_tongue_skin.ifcl[8]";
connectAttr "loElbow_R.obcc" "buddy_tongue_skin.ifcl[9]";
connectAttr "loFingerA1_L.obcc" "buddy_tongue_skin.ifcl[10]";
connectAttr "loFingerA1_R.obcc" "buddy_tongue_skin.ifcl[11]";
connectAttr "loFingerA2_L.obcc" "buddy_tongue_skin.ifcl[12]";
connectAttr "loFingerA2_R.obcc" "buddy_tongue_skin.ifcl[13]";
connectAttr "loFingerB1_L.obcc" "buddy_tongue_skin.ifcl[14]";
connectAttr "loFingerB1_R.obcc" "buddy_tongue_skin.ifcl[15]";
connectAttr "loFingerB2_L.obcc" "buddy_tongue_skin.ifcl[16]";
connectAttr "loFingerB2_R.obcc" "buddy_tongue_skin.ifcl[17]";
connectAttr "loShoulder_L.obcc" "buddy_tongue_skin.ifcl[18]";
connectAttr "loShoulder_R.obcc" "buddy_tongue_skin.ifcl[19]";
connectAttr "loThumb1_L.obcc" "buddy_tongue_skin.ifcl[20]";
connectAttr "loThumb1_R.obcc" "buddy_tongue_skin.ifcl[21]";
connectAttr "loThumb2_L.obcc" "buddy_tongue_skin.ifcl[22]";
connectAttr "loThumb2_R.obcc" "buddy_tongue_skin.ifcl[23]";
connectAttr "loWrist_L.obcc" "buddy_tongue_skin.ifcl[24]";
connectAttr "loWrist_R.obcc" "buddy_tongue_skin.ifcl[25]";
connectAttr "spine.obcc" "buddy_tongue_skin.ifcl[26]";
connectAttr "upElbow_L.obcc" "buddy_tongue_skin.ifcl[27]";
connectAttr "upElbow_R.obcc" "buddy_tongue_skin.ifcl[28]";
connectAttr "upFingerA1_L.obcc" "buddy_tongue_skin.ifcl[29]";
connectAttr "upFingerA1_R.obcc" "buddy_tongue_skin.ifcl[30]";
connectAttr "upFingerA2_L.obcc" "buddy_tongue_skin.ifcl[31]";
connectAttr "upFingerA2_R.obcc" "buddy_tongue_skin.ifcl[32]";
connectAttr "upFingerB1_L.obcc" "buddy_tongue_skin.ifcl[33]";
connectAttr "upFingerB1_R.obcc" "buddy_tongue_skin.ifcl[34]";
connectAttr "upFingerB2_L.obcc" "buddy_tongue_skin.ifcl[35]";
connectAttr "upFingerB2_R.obcc" "buddy_tongue_skin.ifcl[36]";
connectAttr "upShoulder_L.obcc" "buddy_tongue_skin.ifcl[37]";
connectAttr "upShoulder_R.obcc" "buddy_tongue_skin.ifcl[38]";
connectAttr "upThumb1_L.obcc" "buddy_tongue_skin.ifcl[39]";
connectAttr "upThumb1_R.obcc" "buddy_tongue_skin.ifcl[40]";
connectAttr "upThumb2_L.obcc" "buddy_tongue_skin.ifcl[41]";
connectAttr "upThumb2_R.obcc" "buddy_tongue_skin.ifcl[42]";
connectAttr "upWrist_L.obcc" "buddy_tongue_skin.ifcl[43]";
connectAttr "upWrist_R.obcc" "buddy_tongue_skin.ifcl[44]";
connectAttr "bindPose1.msg" "buddy_tongue_skin.bp";
connectAttr "lambert4SG.msg" "materialInfo3.sg";
connectAttr "lambert4.msg" "materialInfo3.m";
connectAttr "lambert4.oc" "lambert4SG.ss";
connectAttr "buddy_tongueShape.iog" "lambert4SG.dsm" -na;
connectAttr "lambert2.oc" "lambert2SG.ss";
connectAttr "groupId19.msg" "lambert2SG.gn" -na;
connectAttr "groupId21.msg" "lambert2SG.gn" -na;
connectAttr "buddy_eye_lShape.iog.og[3]" "lambert2SG.dsm" -na;
connectAttr "buddy_eye_rShape.iog.og[3]" "lambert2SG.dsm" -na;
connectAttr "lambert2SG.msg" "materialInfo1.sg";
connectAttr "lambert2.msg" "materialInfo1.m";
connectAttr "buddy_eye_l_skinGroupParts.og" "buddy_eye_l_skin.ip[0].ig";
connectAttr "buddy_eye_lShapeOrig.o" "buddy_eye_l_skin.orggeom[0]";
connectAttr "bindPose2.msg" "buddy_eye_l_skin.bp";
connectAttr "eye_L.wm" "buddy_eye_l_skin.ma[0]";
connectAttr "eye_L.liw" "buddy_eye_l_skin.lw[0]";
connectAttr "eye_L.obcc" "buddy_eye_l_skin.ifcl[0]";
connectAttr "groupParts1.og" "buddy_eye_l_skinGroupParts.ig";
connectAttr "groupId19.id" "buddy_eye_l_skinGroupParts.gi";
connectAttr "buddy_eye_lShapeOrig.w" "groupParts1.ig";
connectAttr "groupId18.id" "groupParts1.gi";
connectAttr "buddy_eye_r_skinGroupParts.og" "buddy_eye_r_skin.ip[0].ig";
connectAttr "buddy_eye_rShapeOrig.o" "buddy_eye_r_skin.orggeom[0]";
connectAttr "bindPose3.msg" "buddy_eye_r_skin.bp";
connectAttr "eye_R.wm" "buddy_eye_r_skin.ma[0]";
connectAttr "eye_R.liw" "buddy_eye_r_skin.lw[0]";
connectAttr "eye_R.obcc" "buddy_eye_r_skin.ifcl[0]";
connectAttr "groupParts2.og" "buddy_eye_r_skinGroupParts.ig";
connectAttr "groupId21.id" "buddy_eye_r_skinGroupParts.gi";
connectAttr "buddy_eye_rShapeOrig.w" "groupParts2.ig";
connectAttr "groupId20.id" "groupParts2.gi";
relationship "link" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" "lambert2SG.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" "lambert3SG.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" "lambert4SG.message" ":defaultLightSet.message";
relationship "link" ":lightLinker1" "lambert5SG.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialShadingGroup.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" ":initialParticleSE.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" "lambert2SG.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" "lambert3SG.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" "lambert4SG.message" ":defaultLightSet.message";
relationship "shadowLink" ":lightLinker1" "lambert5SG.message" ":defaultLightSet.message";
connectAttr "lambert2SG.pa" ":renderPartition.st" -na;
connectAttr "lambert3SG.pa" ":renderPartition.st" -na;
connectAttr "lambert4SG.pa" ":renderPartition.st" -na;
connectAttr "lambert5SG.pa" ":renderPartition.st" -na;
connectAttr "lambert2.msg" ":defaultShaderList1.s" -na;
connectAttr "lambert3.msg" ":defaultShaderList1.s" -na;
connectAttr "lambert4.msg" ":defaultShaderList1.s" -na;
connectAttr "lambert5.msg" ":defaultShaderList1.s" -na;
// End of buddy_rig.ma

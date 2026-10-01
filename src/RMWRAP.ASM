
;--- include the 16-bit code as binary. This way one avoids possible
;--- problems with a linker's 16-bit fixup handling for 32-bit binaries.

	.386
	.model ?MODEL

;--- OW v1.9 wlink has problems with class "CONST" if output format is PE.
ifdef OW19
	.data
else
	.const
endif

RMCodeTab label dword
	dd offset RMCode1, RMCode1Size
	dd offset RMCode2, RMCode2Size
ifdef LPTBLOB
	dd offset RMCode3, RMCode3Size
endif

RMCode1 label byte
%	incbin <OUTD/rmcode1.bin>
RMCode1Size equ $ - offset RMCode1
RMCode2 label byte
%	incbin <OUTD/rmcode2.bin>
RMCode2Size equ $ - offset RMCode2
;--- rmcode1.asm assembled with LPTSTUB: the /LPT stub (djgpp.mak only)
ifdef LPTBLOB
RMCode3 label byte
%	incbin <OUTD/rmcode3.bin>
RMCode3Size equ $ - offset RMCode3
endif

	.code

;--- size in bytes of blob nSrc (the index copyrmcode takes)
rmcodesize proc c public nSrc:dword
	mov eax, nSrc
	lea eax, [eax*8+RMCodeTab]
	mov eax, [eax+4]
	ret
rmcodesize endp

copyrmcode proc c public uses esi edi pDst:ptr, nSrc:dword
	mov edi, pDst
	mov esi, nSrc
	lea esi, [esi*8+RMCodeTab]
	lodsd
	mov ecx, eax
	lodsd
	mov esi, eax
	xchg esi, ecx
	rep movsb
	mov eax, edi
	ret
copyrmcode endp

	end

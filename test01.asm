start:
	org 100h

	; test instructions 00h to 0fh
	add [scratch], dl
	add [scratch], dx
	add bl, [scratch]
	add cx, [scratch]
	add al, '$'
	add ax, 1234h
	push es
	pop es
	or [scratch], dl
	or [scratch], dx
	or bl, [scratch]
	or cx, [scratch]
	or al, '$'
	or ax, 1234h
	push cs
	; not valid: pop cs

	; test instructions 10h to 1fh
	adc [scratch], dl
	adc [scratch], dx
	adc bl, [scratch]
	adc cx, [scratch]
	adc al, '$'
	adc ax, 1234h
	push ss
	pop ss
	sbb [scratch], dl
	sbb [scratch], dx
	sbb bl, [scratch]
	sbb cx, [scratch]
	sbb al, '$'
	sbb ax, 1234h
	push ds
	pop ds

	; test instructions 20h to 2fh
	and [scratch], dl
	and [scratch], dx
	and bl, [scratch]
	and cx, [scratch]
	and al, '$'
	and ax, 1234h
	add [es:scratch], dl	; test ES override prefix
	daa
	sub [scratch], dl
	sub [scratch], dx
	sub bl, [scratch]
	sub cx, [scratch]
	sub al, '$'
	sub ax, 1234h
	add [cs:scratch], dl	; test CS override prefix
	das

scratch:	dd 12345678h

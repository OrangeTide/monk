	org 100h
	xor ch, ch
	mov cl, [80h]
	mov bx, 1
	mov ah, 40h
	mov dx, 81h
	int 21h
	int 20h

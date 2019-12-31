	org 100h
	movzx cx, byte [80]
	mov bx, 1
	mov ah, 40h
	mov dx, 81h
	int 21h
	int 20h

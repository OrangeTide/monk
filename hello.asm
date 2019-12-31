	org 100h
	mov dx, msg
	mov ah, 09H
	int 21h
	int 20h
msg:	db "Hello, World$"

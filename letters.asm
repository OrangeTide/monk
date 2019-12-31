	org 100h
	mov cx, 1ah
	mov dl, 41h
next:
	mov ah, 2
	int 21h
	inc dl
	loop next
	int 20h

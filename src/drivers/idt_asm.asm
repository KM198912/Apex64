[bits 64]
[extern interrupts_handle_int]
[global isr_table]

%macro pushaq 0
	push rax
	push rcx
	push rdx
	push rbx
	push rbp
	push rsi
	push rdi
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
%endmacro

%macro popaq 0
	pop r15
	pop r14
	pop r13
	pop r12
	pop r11
	pop r10
	pop r9
	pop r8
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	pop rdx
	pop rcx
	pop rax
%endmacro

%macro int_stub 1
int_stub%+%1:
    push %1   ; Push int_no FIRST
    %if !(%1 == 8 || (%1 >= 10 && %1 <= 14) || %1 == 17 || %1 == 21 || %1 == 29 || %1 == 30)
        push 0  ; Then push dummy error code if needed
    %endif
    pushaq
    ; ... rest
    
    mov rdi, rsp              ; Pass context pointer as first argument
    
    ; Save original RSP
    mov rbp, rsp              ; Save in RBP (callee-saved register)
    
    ; Align stack to 16 bytes
    and rsp, ~0xF
    sub rsp, 8
    
    call interrupts_handle_int
    
    ; Restore original stack pointer
    mov rsp, rbp
    
    popaq
    add rsp, 16               ; Remove error code and int_no
    iretq
%endmacro

%assign i 0
%rep 256
int_stub i
%assign i i + 1
%endrep

[section .data]

%assign j 0

isr_table:
	%rep 256
	dq int_stub%+j
	%assign j j + 1
	%endrep
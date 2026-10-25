/* Freestanding Linux x86-64 runtime. Regenerate the checked-in blob with
 * python3 tools/generate_native_runtime.py. No callee-saved register is changed.
 * R12 holds the kernel's initial stack pointer, set by the ELF entry stub. */
.text
.globl rt_print_i32, rt_print_str, rt_str_len, rt_str_eq
.globl rt_str_concat, rt_str_index
.globl rt_file_open, rt_file_open_write, rt_file_close, rt_file_write, rt_file_read
.globl rt_mem_alloc, rt_mem_free, rt_mem_realloc, rt_args_count, rt_args_get

/* Internal write(fd, bytes, length), retrying interruption and short writes. */
.Lwrite_all:
    test %rdx,%rdx
    jz .Lwrite_done
    mov $1,%eax
    syscall
    cmp $-4,%rax
    je .Lwrite_all
    test %rax,%rax
    jle .Lwrite_done
    add %rax,%rsi
    sub %rax,%rdx
    jmp .Lwrite_all
.Lwrite_done:
    ret

rt_print_i32:
    sub $40,%rsp
    lea 39(%rsp),%rsi
    movb $10,(%rsi)
    mov $1,%r8d
    movslq %edi,%rax
    xor %r9d,%r9d
    test %rax,%rax
    jge .Ldecimal
    neg %rax
    mov $1,%r9d
.Ldecimal:
    mov $10,%r10d
.Ldigit:
    xor %edx,%edx
    div %r10
    add $48,%dl
    dec %rsi
    mov %dl,(%rsi)
    inc %r8
    test %rax,%rax
    jnz .Ldigit
    test %r9d,%r9d
    jz .Ldecimal_write
    dec %rsi
    movb $45,(%rsi)
    inc %r8
.Ldecimal_write:
    mov %r8,%rdx
    mov $1,%edi
    call .Lwrite_all
    add $40,%rsp
    ret

rt_str_len:
    xor %eax,%eax
    test %rdi,%rdi
    jz .Llen_done
.Llen_loop:
    cmpb $0,(%rdi,%rax)
    je .Llen_done
    inc %rax
    cmp $2147483647,%rax
    jne .Llen_loop
.Llen_done:
    ret

rt_str_eq:
    cmp %rdi,%rsi
    je .Lequal
    test %rdi,%rdi
    jz .Lunequal
    test %rsi,%rsi
    jz .Lunequal
.Leq_loop:
    movzbl (%rdi),%eax
    cmpb (%rsi),%al
    jne .Lunequal
    inc %rdi
    inc %rsi
    test %al,%al
    jnz .Leq_loop
.Lequal:
    mov $1,%eax
    ret
.Lunequal:
    xor %eax,%eax
    ret

/* Bootstrap strings are NUL-terminated. The public allocator accepts i32
 * sizes, so reject a combined length that cannot include its terminator.
 * Like the C bootstrap helper, allocation failure returns the empty string. */
rt_str_concat:
    sub $56,%rsp
    mov %rdi,(%rsp)
    mov %rsi,8(%rsp)
    call rt_str_len
    mov %rax,16(%rsp)
    mov 8(%rsp),%rdi
    call rt_str_len
    mov %rax,24(%rsp)
    add 16(%rsp),%rax
    inc %rax
    cmp $2147483647,%rax
    ja .Lconcat_empty
    mov %eax,%edi
    call rt_mem_alloc
    test %rax,%rax
    jz .Lconcat_empty
    mov %rax,32(%rsp)
    mov %rax,%rdi
    mov (%rsp),%rsi
    mov 16(%rsp),%rcx
    cld
    rep movsb
    mov 8(%rsp),%rsi
    mov 24(%rsp),%rcx
    rep movsb
    movb $0,(%rdi)
    mov 32(%rsp),%rax
    add $56,%rsp
    ret
.Lconcat_empty:
    lea .Lempty(%rip),%rax
    add $56,%rsp
    ret

/* str_index(str, i32) -> i32. Return an unsigned byte, and trap before
 * reading for negative indices, the NUL terminator, or any later byte. */
rt_str_index:
    sub $24,%rsp
    mov %rdi,(%rsp)
    movslq %esi,%rsi
    mov %rsi,8(%rsp)
    test %rsi,%rsi
    js .Lstr_index_trap
    call rt_str_len
    mov 8(%rsp),%rsi
    cmp %rax,%rsi
    jae .Lstr_index_trap
    mov (%rsp),%rdi
    movzbl (%rdi,%rsi),%eax
    add $24,%rsp
    ret
.Lstr_index_trap:
    ud2

rt_print_str:
    sub $24,%rsp
    mov %rdi,(%rsp)
    call rt_str_len
    mov %rax,%rdx
    mov (%rsp),%rsi
    mov $1,%edi
    call .Lwrite_all
    mov $1,%edi
    lea .Lnewline(%rip),%rsi
    mov $1,%edx
    call .Lwrite_all
    add $24,%rsp
    ret

rt_file_open:
    xor %esi,%esi
    xor %edx,%edx
    jmp .Lopen
rt_file_open_write:
    mov $577,%esi /* O_WRONLY | O_CREAT | O_TRUNC */
    mov $420,%edx /* 0644 */
.Lopen:
    mov $2,%eax
    syscall
    cmp $-4,%rax
    je .Lopen
    test %rax,%rax
    jns .Lopen_done
    mov $-1,%rax
.Lopen_done:
    ret
rt_file_close:
    movslq %edi,%rdi
    mov $3,%eax
    syscall
    ret
rt_file_write:
    sub $24,%rsp
    movslq %edi,%rdi
    mov %rdi,(%rsp)
    mov %rsi,8(%rsp)
    mov %rsi,%rdi
    call rt_str_len
    mov %rax,%rdx
    mov (%rsp),%rdi
    mov 8(%rsp),%rsi
    call .Lwrite_all
    add $24,%rsp
    ret

/* Header: mapping size at -16, requested size at -8. mmap provides alignment.
 * Public allocation size is a signed i32; checked addition guards the header. */
rt_mem_alloc:
    movslq %edi,%rdi
    test %rdi,%rdi
    jle .Lalloc_fail
    mov %rdi,%r8
    add $16,%rdi
    jc .Lalloc_fail
    mov %rdi,%rsi
    xor %edi,%edi
    mov $3,%edx /* PROT_READ | PROT_WRITE */
    mov $34,%r10d /* MAP_PRIVATE | MAP_ANONYMOUS */
    mov %r8,%r9 /* retain requested size across syscall */
    mov $-1,%r8
    push %r9
    xor %r9d,%r9d
    mov $9,%eax
    syscall
    pop %r9
    cmp $-4095,%rax
    jae .Lalloc_fail
    mov %rsi,(%rax)
    mov %r9,8(%rax)
    add $16,%rax
    ret
.Lalloc_fail:
    xor %eax,%eax
    ret
rt_mem_free:
    test %rdi,%rdi
    jz .Lfree_done
    sub $16,%rdi
    mov (%rdi),%rsi
    mov $11,%eax
    syscall
.Lfree_done:
    ret
rt_mem_realloc:
    sub $40,%rsp
    mov %rdi,(%rsp)
    movslq %esi,%rsi
    mov %rsi,8(%rsp)
    test %rsi,%rsi
    jle .Lrealloc_zero
    mov %rsi,%rdi
    call rt_mem_alloc
    test %rax,%rax
    jz .Lrealloc_return
    mov %rax,16(%rsp)
    mov (%rsp),%rsi
    test %rsi,%rsi
    jz .Lrealloc_result
    mov -8(%rsi),%rcx
    cmp 8(%rsp),%rcx
    jbe .Lrealloc_copy
    mov 8(%rsp),%rcx
.Lrealloc_copy:
    mov %rax,%rdi
    cld
    rep movsb
    mov (%rsp),%rdi
    call rt_mem_free
.Lrealloc_result:
    mov 16(%rsp),%rax
.Lrealloc_return:
    add $40,%rsp
    ret
.Lrealloc_zero:
    call rt_mem_free
    xor %eax,%eax
    jmp .Lrealloc_return

/* Read from the current position; grow rather than silently truncating.
 * Locals: fd, pointer, capacity, used. Allocation includes the terminator. */
rt_file_read:
    sub $40,%rsp
    movslq %edi,%rdi
    mov %rdi,(%rsp)
    movq $4096,16(%rsp)
    movq $0,24(%rsp)
    mov $4097,%edi
    call rt_mem_alloc
    mov %rax,8(%rsp)
    test %rax,%rax
    jz .Lread_empty
.Lread_loop:
    mov 16(%rsp),%rdx
    sub 24(%rsp),%rdx
    jz .Lread_grow
    mov 8(%rsp),%rsi
    add 24(%rsp),%rsi
    mov (%rsp),%rdi
    xor %eax,%eax
    syscall
    cmp $-4,%rax
    je .Lread_loop
    test %rax,%rax
    js .Lread_error
    jz .Lread_done
    add %rax,24(%rsp)
    jmp .Lread_loop
.Lread_grow:
    mov 16(%rsp),%rsi
    /* Maximum representable allocation leaves one byte for NUL. */
    cmp $2147483646,%rsi
    jae .Lread_error
    add %rsi,%rsi
    cmp $2147483646,%rsi
    jbe .Lread_grow_size
    mov $2147483646,%esi
.Lread_grow_size:
    mov %rsi,16(%rsp)
    inc %rsi
    mov 8(%rsp),%rdi
    call rt_mem_realloc
    test %rax,%rax
    jz .Lread_error
    mov %rax,8(%rsp)
    jmp .Lread_loop
.Lread_done:
    mov 8(%rsp),%rax
    mov 24(%rsp),%rdx
    movb $0,(%rax,%rdx)
    add $40,%rsp
    ret
.Lread_error:
    mov 8(%rsp),%rdi
    call rt_mem_free
.Lread_empty:
    lea .Lempty(%rip),%rax
    add $40,%rsp
    ret
rt_args_count:
    mov (%r12),%rax
    cmp $2147483647,%rax
    jbe .Largc_done
    mov $2147483647,%eax
.Largc_done:
    ret
rt_args_get:
    movslq %edi,%rdi
    test %rdi,%rdi
    js .Largs_empty
    cmp (%r12),%rdi
    jae .Largs_empty
    mov 8(%r12,%rdi,8),%rax
    ret
.Largs_empty:
    lea .Lempty(%rip),%rax
    ret
.Lnewline:
    .byte 10
.Lempty:
    .byte 0
.section .note.GNU-stack,"",@progbits

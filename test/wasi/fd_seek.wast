(module
  (import "wasi_snapshot_preview1" "path_open" (func $path_open (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_write" (func $wasi_fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_read" (func $wasi_fd_read (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "fd_seek" (func $wasi_fd_seek (param i32 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit" (func $proc_exit (param i32)))
  
 
  (memory 1)
  
  (export "memory" (memory 0))
  (export "read_from_file_with_seek" (func $read_from_file_with_seek))
  (data (i32.const 300) "./text.txt")
 
  (; 
    This test searches for the file 'text.txt' in the first opened directory
    and reads 40 characters out of it.
    It will only read from the file if the directories were mapped correctly.
  ;)
 
  (func $read_from_file_with_seek (param i64 i32)
    i32.const 3 ;; Directory file descriptior, by default 3 is the first opened directory
    i32.const 1 ;;lookupflags: directory
    i32.const 300 ;; Offset of file name in memory
    i32.const 19 ;; Length of file name
    i32.const 0 ;; oflags: none
    i64.const 4102 ;; rights: path_open, fd_read, fd_seek
    i64.const 4102 ;; rights_inheriting: path_open, fd_read
    i32.const 0 ;; fdflags: none
    i32.const 0 ;; Offset to store at the opened file descriptor in memory
    call $path_open
    
    i32.eqz ;; fail if file could not be opened
    (if
      (then)
      (else
        i32.const 1
        call $proc_exit
      )
    )
 
    (i32.store (i32.const 104) (i32.const 6))
    (i32.store (i32.const 100) (i32.const 0))
    (i32.store (i32.const 200) (i32.const 500))

    (call $wasi_fd_seek
      (i32.const 0)
      (i32.load)
      (local.get 0)
      (local.get 1)
      (i32.const 1000)
    )
    drop
 
    (call $wasi_fd_read
      (i32.const 0)
      (i32.load) ;; opened file descriptor
      
      (i32.const 100) ;; store content at this location
      (i32.const 1) ;; make it into a single buffer
      (i32.const 200) ;; store number of read characters to this location
    )
    drop
 
    (call $wasi_fd_write
      (i32.const 1)  ;;file descriptor
      (i32.const 100) ;;offset of str offset
      (i32.const 1)  ;;iovec length
      (i32.const 200) ;;result offset
    )
    drop
  )
)
 
(assert_return (invoke "read_from_file_with_seek" (i64.const 6) (i32.const 0) ))
(assert_return (invoke "read_from_file_with_seek" (i64.const -6) (i32.const 0) ))

(assert_return (invoke "read_from_file_with_seek" (i64.const 6) (i32.const 1) ))
(assert_return (invoke "read_from_file_with_seek" (i64.const -6) (i32.const 1) ))

(assert_return (invoke "read_from_file_with_seek" (i64.const -12) (i32.const 2) ))
(assert_return (invoke "read_from_file_with_seek" (i64.const -6) (i32.const 2) ))


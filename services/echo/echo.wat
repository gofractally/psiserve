(module
  ;; Host imports (must come first)
  (import "psi" "accept" (func $psi_accept (param i32) (result i32)))
  (import "psi" "read"   (func $psi_read   (param i32 i32 i32) (result i32)))
  (import "psi" "write"  (func $psi_write  (param i32 i32 i32) (result i32)))
  (import "psi" "close"  (func $psi_close  (param i32)))

  ;; Memory: 1 page (64KB), buffer at offset 0
  (memory (export "memory") 1)

  (func $start (export "_start")
    (local $conn i32)
    (local $n i32)

    ;; Accept loop
    (block $quit
      (loop $accept_loop
        ;; conn = psi_accept(0)
        (local.set $conn (call $psi_accept (i32.const 0)))

        ;; if conn < 0, quit
        (br_if $quit (i32.lt_s (local.get $conn) (i32.const 0)))

        ;; Echo loop
        (block $done
          (loop $echo_loop
            ;; n = psi_read(conn, buf=0, len=4096)
            (local.set $n
              (call $psi_read
                (local.get $conn)
                (i32.const 0)    ;; buffer at offset 0
                (i32.const 4096)
              )
            )

            ;; if n <= 0, break
            (br_if $done (i32.le_s (local.get $n) (i32.const 0)))

            ;; psi_write(conn, buf=0, n)
            (drop
              (call $psi_write
                (local.get $conn)
                (i32.const 0)
                (local.get $n)
              )
            )

            (br $echo_loop)
          )
        )

        ;; psi_close(conn)
        (call $psi_close (local.get $conn))

        (br $accept_loop)
      )
    )
  )
)

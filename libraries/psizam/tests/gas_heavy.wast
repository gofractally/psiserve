(module
  (func (export "loop_div") (param i64 i64 i32) (result i64)
    (local i64) ;; local 3: accumulator
    (local.set 3 (local.get 0))
    (block $exit
      (loop $top
        (br_if $exit (i32.eqz (local.get 2)))
        (local.set 3 (i64.div_s (local.get 3) (local.get 1)))
        (local.set 3 (i64.add  (local.get 3) (local.get 0)))
        (local.set 2 (i32.sub  (local.get 2) (i32.const 1)))
        (br $top)
      )
    )
    (local.get 3)
  )
)

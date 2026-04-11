# ── chat-bench: latency benchmark for the chat server ──────────────────────────

add_executable(chat-bench
   main.cpp
)

target_link_libraries(chat-bench PRIVATE
   Threads::Threads
)

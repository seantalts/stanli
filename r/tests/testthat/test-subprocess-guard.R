test_that("subprocess tests skip where R cannot start another process", {
  expect_false(subprocess_available(os = "emscripten4.0.2"))
  expect_false(subprocess_available(bin = tempfile("no-r-bin")))
  expect_true(subprocess_available())
  expect_condition(skip_if_no_subprocess(os = "emscripten"), class = "skip")
  expect_no_condition(skip_if_no_subprocess())
})

test_that("thread tests skip on a runtime without threads", {
  local_mocked_bindings(stanli_thread_safe = function() FALSE)
  expect_condition(skip_without_threads(), class = "skip")
  local_mocked_bindings(stanli_thread_safe = function() TRUE)
  expect_no_condition(skip_without_threads())
})

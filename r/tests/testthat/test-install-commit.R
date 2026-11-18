release_sha <- "6537b0ba3c7088a696861e86e2d70943ed7e9fc1"
later_sha <- "5161f587113091f12c6539eb86d653c0da3c96e1"
release_build <- "abi6-6537b0ba3c70-Darwin-arm64-threads"

test_that("the runtime's commit comes from its build id", {
  expect_identical(stanli:::runtime_commit(release_build), "6537b0ba3c70")
  expect_identical(stanli:::runtime_commit("not a build id"), NA_character_)
})

test_that("a package installed from the runtime's own commit gets no note", {
  expect_null(stanli:::commit_mismatch_note(release_sha, "6537b0ba3c70"))
})

test_that("a package without a recorded commit gets no note", {
  expect_null(stanli:::commit_mismatch_note(NA_character_, "6537b0ba3c70"))
  expect_null(stanli:::commit_mismatch_note(later_sha, NA_character_))
})

test_that("a package from a later commit names both commits", {
  note <- stanli:::commit_mismatch_note(later_sha, "6537b0ba3c70")
  expect_match(note, "5161f5871130", fixed = TRUE)
  expect_match(note, "6537b0ba3c70", fixed = TRUE)
  expect_match(note, "seantalts/stanli@*release", fixed = TRUE)
})

test_that("a failed build carries the note", {
  note <- stanli:::commit_mismatch_note(later_sha, "6537b0ba3c70")
  expect_error(stanli:::with_commit_note(stop("boom"), note),
               "boom.*5161f5871130")
  expect_error(stanli:::with_commit_note(stop("boom"), NULL), "^boom$")
  expect_identical(stanli:::with_commit_note(1 + 1, note), 2)
})

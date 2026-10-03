# doctest discovery fixture

`scripts/cmake/doctestAddTests.cmake` is the unmodified upstream file from
doctest v2.5.3, commit `2d0a9359a60c51affe2a9bebb1be1dca47868151`:

https://github.com/doctest/doctest/blob/2d0a9359a60c51affe2a9bebb1be1dca47868151/scripts/cmake/doctestAddTests.cmake

The archive is pinned by `dependencies.json`; its SHA-256 is
`e64542c4ea68e9f381ccf6eae924cfdd652567c87c142d76fe92644fb4608149`.
The discovery file's SHA-256 is
`3ec2839f12b7a12b0d7ef6244b60eecae40ad797caa3f016fc09875956a518dc`.
The upstream license file and the module's BSD license notice are retained.

The offline cache integration test packages this module in its minimal
doctest archive. The production recipe must apply its UTF-8 patch and install
the module successfully. The test then runs that installed module with UTF-8
case names and suite labels, checks the child-process decoding arguments on
all platforms, and executes the generated CTest cases. Keep this fixture
unpatched so patch drift remains a test failure.

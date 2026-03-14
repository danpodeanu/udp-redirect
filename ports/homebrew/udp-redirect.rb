class UdpRedirect < Formula
  desc "Simple yet flexible and very fast UDP redirector"
  homepage "https://github.com/danpodeanu/udp-redirect"
  url "https://github.com/danpodeanu/udp-redirect/archive/refs/tags/v2.2.0.tar.gz"
  sha256 "f9856d8d84cd7eec62394be400525c12713c0fa28735746691568a21e3e60fbb"
  license "GPL-2.0-only"

  head do
    url "https://github.com/danpodeanu/udp-redirect.git", branch: "main"
    depends_on "autoconf" => :build
    depends_on "automake" => :build
    depends_on "libtool" => :build
  end

  def install
    # system "make", "install", "PREFIX=#{prefix}", "CC=#{ENV.cc}"
    system "make"

    bin.install "udp-redirect"
    man1.install "udp-redirect.1"
  end

  test do
    system "#{bin}/udp-redirect", "--version"
  end
end

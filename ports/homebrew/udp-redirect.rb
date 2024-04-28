class UdpRedirect < Formula
  desc "Simple yet flexible and very fast UDP redirector"
  homepage "https://github.com/danpodeanu/udp-redirect"
  url "https://github.com/danpodeanu/udp-redirect/archive/refs/tags/v0.9.1.tar.gz"
  sha256 "4c72820ccbc19bf4d8d5a6a9dd8a75894d54cac6ee591278f7074117aa7a3cfe"
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

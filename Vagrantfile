Vagrant.configure("2") do |config|
  config.vm.box = "bento/ubuntu-24.04"
  config.vm.box_download_insecure = true
  config.vm.hostname = "simplefs-dev"

  config.vm.provider "virtualbox" do |vb|
    vb.name = "simplefs-dev"
    vb.memory = 2048
    vb.cpus = 2
  end

  config.vm.provision "shell", inline: <<-SHELL
    set -eu
    apt-get update
    apt-get install -y build-essential linux-headers-$(uname -r) kmod util-linux
  SHELL
end

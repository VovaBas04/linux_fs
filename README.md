## Инструкция
Собирать проект нужно внутри Linux с установленными заголовками ядра. Для задания нужна версия ядра `6.12.x`.

```bash
make
```

После сборки появятся:
- `src/simplefs.ko` — модуль ядра;
- `simplefsctl` — userspace-утилита для проверки и ioctl.

## Запуск через Ubuntu VM

В проекте есть `Vagrantfile`, чтобы поднять Ubuntu VM.

На хосте:

```bash
vagrant up
vagrant ssh
```

Внутри VM:

```bash
cd /vagrant
make
sudo apt-get install linux-headers-$(uname -r)
```

Пример комманды запуска:

```bash
sudo ./simplefsctl demo /mnt/simplefs
```
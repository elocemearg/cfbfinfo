# cfbfinfo
Scan, list and examine Microsoft CFB files, and extract Publisher text

# Introduction
cfbfinfo is a tool for examining Microsoft CFB file (Compound File Binary File) format files. This is the format used by Microsoft Publisher and some older versions of Word. It came in handy when a Publisher file rendered itself unopenable and the text needed to be recovered.

Note that the specific format details for Word and Publisher are proprietary and not publicly known. However, the container format (CFB) is well-documented. Furthermore, all the text, but not formatting, tables, or anything else, is stored in a single stream inside a Publisher file.

# Compatibility
This program has only been run and tested on Linux. It may be compatible with other environments.

# Build
Run `make cfbfinfo` and the executable file `cfbfinfo` will be compiled.

# Extracting the text from an MS Publisher file

The following command extracts the text from the Publisher file `mypublisherfile.pub` and write it to `mytext.txt`.

```
cfbfinfo -t mypublisherfile.pub -o mytext.txt
```

# Help
Run `cfbfinfo` without arguments for a list of options.

# References
[MS-CFB: Compound File Binary File Format](https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-cfb/53989ce4-7b05-4f8d-829b-d08d6148375b) - Microsoft's documentation on the container format.

# Frequently asked questions

------

[How do I change my FPS?](#how-do-i-change-my-fps)

[How do I submit an issue?](#how-do-i-submit-an-issue)


-----

### How do I change my FPS?


Go to $HOME/.config/mocktail, there is a file called config.yaml (appears after opening for the first time mocktail)


<img width="460" height="241" alt="image" src="https://github.com/user-attachments/assets/b30b1fea-4937-4858-937a-3096a9e8b2ad" />


with your text editor of preference search for "graphics" 


<img width="1022" height="619" alt="image" src="https://github.com/user-attachments/assets/f86791e9-7ac3-4d13-b81b-3cf9e3e64c3d" />


change the display value to your desired fps, also turn off vsync if you want more fps than your display supports

---

### How do I submit an issue?

To make a more standard way so this don't becomes a mess you should follow this to make an issue:

First check if the issue already exists or has been solved, we want to avoid duplicates so we don't talk about some issue in a lot of places, if you are sure there is no issue related to you then you can follow this template
```text
What have you done before this issue:


Problem Description:


Steps to replicate:


Environment:

```

And an example on how a good issue would look:

```text
What have you done before this issue:
I have searched if there was a similar issue and didn't found anything, I have already tried to build from source, installing from flatpak, using the AUR but nothing

Problem Description:
When I write text in the chat it dissapears until I click something else

Steps to replicate:
1.Open mocktail in any version(mocktail, mocktail-git and the flatpak version)
2.Enter any game
3.Write in chat

Environment:
OS:Arch Linux
desktop:KDE plasma
CPU: Intel Pentium Processor 1405
GPU: NVIDIA GeForce RTX 6090
```

Remember if its a duplicate it could get closed so keep that in mind before you make a whole, well written issue

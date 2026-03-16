"ByteArray kernel methods.
 Primitives installed by bootstrap: at: (60), at:put: (61),
 basicAt: (60), basicAt:put: (61), basicSize (62).
 size inherited from ArrayedCollection (prim 53, byte-aware)."

!ByteArray methodsFor: 'printing'!
printString
    | sz result i |
    sz := self size.
    result := '#['.
    i := 1.
    [i <= sz] whileTrue: [
        i > 1 ifTrue: [result := result, ' '].
        result := result, (self at: i) printString.
        i := i + 1].
    result := result, ']'.
    ^ result! !

!ByteArray methodsFor: 'converting'!
asString
    "Interpret bytes as character codes and return a String."
    | sz result i |
    sz := self size.
    result := String new: sz.
    i := 1.
    [i <= sz] whileTrue: [
        result at: i put: (Character value: (self at: i)).
        i := i + 1].
    ^ result! !

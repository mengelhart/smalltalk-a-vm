Object subclass: #TestPoint
    instanceVariableNames: 'x y'
    classVariableNames: ''
    poolDictionaries: ''
    category: ''!

!TestPoint methodsFor: 'accessing'!
x
    ^ x
! !

!TestPoint methodsFor: 'accessing'!
x: aValue
    x := aValue
! !

!TestPoint methodsFor: 'accessing'!
y
    ^ y
! !

!TestPoint methodsFor: 'accessing'!
y: aValue
    y := aValue
! !

!TestPoint methodsFor: 'arithmetic'!
distanceSquaredTo: aPoint
    | dx dy |
    dx := self x - aPoint x.
    dy := self y - aPoint y.
    ^ dx * dx + (dy * dy)
! !
